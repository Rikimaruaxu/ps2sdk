/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/**
 * @file
 * USB Keyboard Driver for PS2
 */

#include "types.h"
#include "ioman.h"
#include "loadcore.h"
#include "stdio.h"
#include "sifcmd.h"
#include "sifrpc.h"
#include "sysclib.h"
#include "sysmem.h"
#include "usbd.h"
#include "usbd_macro.h"
#include "thbase.h"
#include "thevent.h"
#include "thsemap.h"

#include "ps2kbd.h"
#include "us_keymap.h"

#define PS2KBD_VERSION 0x100

#define USB_SUBCLASS_BOOT 1
#define USB_HIDPROTO_KEYBOARD 1

#define PS2KBD_MAXDEV 2
#define PS2KBD_MAXKEYS 6

#define PS2KBD_DEFLINELEN 4096
#define PS2KBD_DEFREPEATRATE 100
/** Default repeat rate in milliseconds */
#define PS2KBD_REPEATWAIT 1000
/** Number of milliseconds to wait before starting key repeat */
#define USB_KEYB_NUMLOCK 0x53
#define USB_KEYB_CAPSLOCK 0x39
#define USB_KEYB_SCRLOCK 0x47

#define USB_KEYB_NUMPAD_START 0x54
#define USB_KEYB_NUMPAD_END 0x63

#define SEMA_ZERO -419
#define SEMA_DELETED -425

int ps2kbd_init();
void ps2kbd_config_set(int resultCode, int bytes, void *arg);
void ps2kbd_idlemode_set(int resultCode, int bytes, void *arg);
void ps2kbd_data_recv(int resultCode, int bytes, void *arg);
int ps2kbd_probe(int devId);
int ps2kbd_connect(int devId);
int ps2kbd_disconnect(int devId);
void usb_getstring(int endp, int index, char *desc);

typedef struct _kbd_data_recv

{
  u8 mod_keys;
  u8 reserved;
  u8 keycodes[PS2KBD_MAXKEYS];
} kbd_data_recv;

typedef struct _keyb_dev

{
  int configEndp;
  int dataEndp;
  int packetSize;
  int devId;
  /** Holds the interface number selected on this device */
  int interfaceNo;    
  char repeatkeys[2];
  u32 eventmask;
  /** Maintains state on the led status */
  u8 ledStatus;     
  kbd_data_recv oldData;
  /** Holds the data for the transfers */
  kbd_data_recv data; 
} kbd_dev;

/* Global Variables */

int kbd_readmode;
int kbd_blocking;
u32 kbd_repeatrate;
/** Holds a list of current devices */
kbd_dev *devices[PS2KBD_MAXDEV]; 
int dev_count;
sceUsbdLddOps kbd_driver = { NULL, NULL, "PS2Kbd", ps2kbd_probe, ps2kbd_connect, ps2kbd_disconnect, 0, 0, 0, 0, 0, NULL };
u8 *lineBuffer;
u32 lineStartP, lineEndP;
int lineSema;
int bufferSema;
u32 lineSize;
/** Normal key map */
u8 keymap[PS2KBD_KEYMAP_SIZE];        
/** Shifted key map */ 
u8 shiftkeymap[PS2KBD_KEYMAP_SIZE];  
/** Does this key get shifted by capslock ? */
u8 keycap[PS2KBD_KEYMAP_SIZE];          
u8 special_keys[PS2KBD_KEYMAP_SIZE];
u8 control_map[PS2KBD_KEYMAP_SIZE];
u8 alt_map[PS2KBD_KEYMAP_SIZE];
//static struct fileio_driver kbd_fdriver;
u8 keyModValue[8] = { 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7 };
int repeat_tid;
int eventid;   /* Id of the repeat event */

int _start (int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  ps2kbd_init();

  printf("PS2KBD - USB Keyboard Library\n");

  return MODULE_RESIDENT_END;
}

int ps2kbd_probe(int devId)

{
  UsbDeviceDescriptor *dev;
  UsbConfigDescriptor *conf;
  UsbInterfaceDescriptor *intf;
  UsbEndpointDescriptor *endp;
  //UsbStringDescriptor *str;

  if(dev_count >= PS2KBD_MAXDEV)
    {
      printf("ERROR: Maximum keyboard devices reached\n");
      return 0;
    }

  //printf("PS2Kbd_probe devId %d\n", devId);

  dev = sceUsbdScanStaticDescriptor(devId, NULL, USB_DT_DEVICE); /* Get device descriptor */
  if(!dev)
    {
      printf("ERROR: Couldn't get device descriptor\n");
      return 0;
    }

  //printf("Device class %d, Size %d, Man %d, Product %d Cpnfigurations %d\n", dev->bDeviceClass, dev->bMaxPacketSize0, dev->iManufacturer, dev->iProduct, dev->bNumConfigurations);
  /* Check that the device class is specified in the interfaces and it has at least one configuration */
  if((dev->bDeviceClass != USB_CLASS_PER_INTERFACE) || (dev->bNumConfigurations < 1))
    {
      //printf("This is not the droid you're looking for\n");
      return 0;
    }

  conf = sceUsbdScanStaticDescriptor(devId, dev, USB_DT_CONFIG);
  if(!conf)
    {
      printf("ERROR: Couldn't get configuration descriptor\n");
      return 0;
    }
  //printf("Config Length %d Total %d Interfaces %d\n", conf->bLength, conf->wTotalLength, conf->bNumInterfaces);

  if((conf->bNumInterfaces < 1) || (conf->wTotalLength < (sizeof(UsbConfigDescriptor) + sizeof(UsbInterfaceDescriptor))))
    {
      printf("ERROR: No interfaces available\n");
      return 0;
    }

  intf = (UsbInterfaceDescriptor *) ((char *) conf + conf->bLength); /* Get first interface */
/*   printf("Interface Length %d Endpoints %d Class %d Sub %d Proto %d\n", intf->bLength, */
/* 	 intf->bNumEndpoints, intf->bInterfaceClass, intf->bInterfaceSubClass, */
/* 	 intf->bInterfaceProtocol); */

  if((intf->bInterfaceClass != USB_CLASS_HID) || (intf->bInterfaceSubClass != USB_SUBCLASS_BOOT) ||
     (intf->bInterfaceProtocol != USB_HIDPROTO_KEYBOARD) || (intf->bNumEndpoints < 1))

    {
      //printf("We came, we saw, we told it to fuck off\n");
      return 0;
    }

  endp = (UsbEndpointDescriptor *) ((char *) intf + intf->bLength);
  endp = (UsbEndpointDescriptor *) ((char *) endp + endp->bLength); /* Go to the data endpoint */

  //printf("Endpoint 1 Addr %d, Attr %d, MaxPacket %d\n", endp->bEndpointAddress, endp->bmAttributes, endp->wMaxPacketSizeLB);

  if(((endp->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT) ||
     ((endp->bEndpointAddress & USB_ENDPOINT_DIR_MASK) != USB_DIR_IN))
    {
      printf("ERROR: Endpoint not interrupt type and/or an input\n");
      return 0;
    }

  printf("PS2KBD: Found a keyboard device\n");

  return 1;
}

int ps2kbd_connect(int devId)

{
  /* Assume we can only get here if we have already checked the device is kosher */

  UsbDeviceDescriptor *dev;
  UsbConfigDescriptor *conf;
  UsbInterfaceDescriptor *intf;
  UsbEndpointDescriptor *endp;
  kbd_dev *currDev;
  int devLoop;

  //printf("PS2Kbd_connect devId %d\n", devId);

  dev = sceUsbdScanStaticDescriptor(devId, NULL, USB_DT_DEVICE); /* Get device descriptor */
  if(!dev)
    {
      printf("ERROR: Couldn't get device descriptor\n");
      return 1;
    }

  conf = sceUsbdScanStaticDescriptor(devId, dev, USB_DT_CONFIG);
  if(!conf)
    {
      printf("ERROR: Couldn't get configuration descriptor\n");
      return 1;
    }

  intf = (UsbInterfaceDescriptor *) ((char *) conf + conf->bLength); /* Get first interface */
  endp = (UsbEndpointDescriptor *) ((char *) intf + intf->bLength);
  endp = (UsbEndpointDescriptor *) ((char *) endp + endp->bLength); /* Go to the data endpoint */

  for(devLoop = 0; devLoop < PS2KBD_MAXDEV; devLoop++)
    {
      if(devices[devLoop] == NULL)
	{
	  break;
	}
    }

  if(devLoop == PS2KBD_MAXDEV)
    {
      /* How the f*** did we end up here ??? */
      printf("ERROR: Device Weirdness!!\n");
      return 1;
    }

  currDev = (kbd_dev *) AllocSysMemory(0, sizeof(kbd_dev), NULL);
  if(!currDev)
    {
      printf("ERROR: Couldn't allocate a device point for the kbd\n");
      return 1;
    }

  devices[devLoop] = currDev;
  memset(currDev, 0, sizeof(kbd_dev));
  currDev->configEndp = sceUsbdOpenPipe(devId, NULL);
  currDev->dataEndp = sceUsbdOpenPipe(devId, endp);
  currDev->packetSize = endp->wMaxPacketSizeLB | ((int) endp->wMaxPacketSizeHB << 8);
  currDev->eventmask = (1 << devLoop);
  if((unsigned int)(currDev->packetSize) > sizeof(kbd_data_recv))
    {
      currDev->packetSize = sizeof(kbd_data_recv);
    }

  if(dev->iManufacturer != 0)
    {
      usb_getstring(currDev->configEndp, dev->iManufacturer, "Keyboard Manufacturer");
    }

  if(dev->iProduct != 0)
    {
      usb_getstring(currDev->configEndp, dev->iProduct, "Keyboard Product");
    }

  currDev->devId = devId;
  currDev->interfaceNo = intf->bInterfaceNumber;
  currDev->ledStatus = 0;

  sceUsbdSetPrivateData(devId, currDev); /* Set the index for the device data */

  //printf("Configuration value %d\n", conf->bConfigurationValue);
  sceUsbdSetConfiguration(currDev->configEndp, conf->bConfigurationValue, ps2kbd_config_set, currDev);

  dev_count++; /* Increment device count */
  printf("PS2KBD: Connected device\n");

  return 0;
}

int ps2kbd_disconnect(int devId)

{
  int devLoop;
  //printf("PS2Kbd_disconnect devId %d\n", devId);

  for(devLoop = 0; devLoop < PS2KBD_MAXDEV; devLoop++)
    {
      if((devices[devLoop]) && (devices[devLoop]->devId == devId))
	{
	  dev_count--;
	  FreeSysMemory(devices[devLoop]);
	  devices[devLoop] = NULL;
	  printf("PS2KBD: Disconnected device\n");
	  break;
	}
    }

  return 0;
}

typedef struct _string_descriptor

{
  u8 buf[200];
  char *desc;
} string_descriptor;

void ps2kbd_getstring_set(int resultCode, int bytes, void *arg)

{
  UsbStringDescriptor *str = (UsbStringDescriptor *) arg;
  string_descriptor *strBuf = (string_descriptor *) arg;

/*   printf("=========getstring=========\n"); */

/*   printf("PS2KEYBOARD: GET_DESCRIPTOR res %d, bytes %d, arg %p\n", resultCode, bytes, arg); */

  if(resultCode == USB_RC_OK)
    {
      char string[50];
      int strLoop;

      memset(string, 0, 50);
      for(strLoop = 0; strLoop < ((bytes - 2) / 2); strLoop++)
	{
	  string[strLoop] = str->wData[strLoop] & 0xFF;
	}
      printf("PS2KBD %s: %s\n", strBuf->desc, string);
    }

  FreeSysMemory(arg);
}

void usb_getstring(int endp, int index, char *desc)

{
  u8 *data;
  string_descriptor *str;

  data = (u8 *) AllocSysMemory(0, sizeof(string_descriptor), NULL);
  str = (string_descriptor *) data;

  if(data != NULL)
    {
      int ret;

      str->desc = desc;
      ret = sceUsbdControlTransfer(endp, 0x80, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) | index,
			       0x0409, sizeof(string_descriptor) - 4, data, ps2kbd_getstring_set, data);
      if(ret != USB_RC_OK)
	{
	  printf("PS2KBD: Error sending string descriptor request\n");
	  FreeSysMemory(data);
	}
    }
}

void ps2kbd_config_set(int resultCode, int bytes, void *arg)
     /* Called when we have finished choosing our configuration */

{
  kbd_dev *dev;

  if(resultCode != USB_RC_OK)
    {
      printf("PS2KEYBOARD: Configuration set error res %d, bytes %d, arg %p\n", resultCode, bytes, arg);
      return;
    }

  //printf("PS2KEYBOARD: Configuration set res %d, bytes %d, arg %p\n", resultCode, bytes, arg);
  /* Do a interrupt data transfer */

  dev = (kbd_dev *) arg;
  if(dev != NULL)
    {
      sceUsbdControlTransfer(dev->configEndp, 0x21, USB_REQ_SET_IDLE, 0, dev->interfaceNo, 0, NULL, ps2kbd_idlemode_set, arg);
    }
}

void ps2kbd_idlemode_set(int resultCode, int bytes, void *arg)

{
  kbd_dev *dev;



  if(resultCode != USB_RC_OK)
    {
      printf("PS2KBD: Idlemode set error res %d, bytes %d, arg %p\n", resultCode, bytes, arg);
      return;
    }

  dev = (kbd_dev *) arg;
  if(dev != NULL)
    {
      sceUsbdInterruptTransfer(dev->dataEndp, &dev->data, dev->packetSize, ps2kbd_data_recv, arg);
    }
}

void ps2kbd_led_set(int resultCode, int bytes, void *arg)

{
  (void)resultCode;
  (void)bytes;
  (void)arg;

  //printf("LED Set\n");
}

void ps2kbd_build_uniquekeys(u8 *res, const u8 *new, const u8 *old)

     /* Builds a list of unique keys */

{
  int loopNew, loopOld;
  int loopRes = 0;
  int foundKey;

  for(loopNew = 0; loopNew < PS2KBD_MAXKEYS; loopNew++)
    {
      if(new[loopNew] != 0)
	{
	  foundKey = 0;
	  for(loopOld = 0; loopOld < PS2KBD_MAXKEYS; loopOld++)
	    {
	      if(new[loopNew] == old[loopOld])
		{
		  foundKey = 1;
		  break;
		}
	    }
	  if(!foundKey)
	    {
	      res[loopRes++] = new[loopNew];
	    }
	}
    }
}

u32 ps2kbd_repeathandler(void *arg)

{
  kbd_dev *dev = arg;
  iop_sys_clock_t t;
  //printf("Repeat handler\n");

  iSetEventFlag(eventid, dev->eventmask);

  USec2SysClock(kbd_repeatrate * 1000, &t);
  iSetAlarm(&t, (void *)ps2kbd_repeathandler, arg);

  return t.hi;
}

void ps2kbd_getkeys(u8 keyMods, u8 ledStatus, const u8 *keys, kbd_dev *dev)

{
  int loopKey;
  int tempPos = 0;
  int byteCount = 0;
  u8 currChars[2];

  if(lineStartP < lineEndP)
    {
      tempPos = lineStartP + lineSize;
    }
  else
    {
      tempPos = lineStartP;
    }

  for(loopKey = 0; loopKey < PS2KBD_MAXKEYS; loopKey++)
    {
      u8 currKey = keys[loopKey];

      currChars[0] = 0;
      currChars[1] = 0;

      if(lineEndP == (u32)(tempPos - 1))
	{
	  break;
	}

      if(currKey) /* If this is a valid key */
	{
	  if((currKey >= USB_KEYB_NUMPAD_START) && (currKey <= USB_KEYB_NUMPAD_END))
	    /* Handle numpad specially */
	    {
	      if(ledStatus & PS2KBD_LED_NUMLOCK)
		{
		  if(keymap[currKey])
		    {
		      currChars[0] = keymap[currKey];
		    }
		}
	      else
		{
		  if(special_keys[currKey])
		    {
		      currChars[0] = PS2KBD_ESCAPE_KEY;
		      currChars[1] = special_keys[currKey];
		    }
		  else if(keymap[currKey] != '5') /* Make sure this isnt a 5 key :) */
		    {
		      currChars[0] = keymap[currKey];
		    }
		}
	    }
	  else if(special_keys[currKey]) /* This is a special key */
	    {
	      currChars[0] = PS2KBD_ESCAPE_KEY;
	      currChars[1] = special_keys[currKey];
	    }
	  else if(keyMods & PS2KBD_CTRL) /* CTRL */
	    {
	      if(control_map[currKey])
		{
		  currChars[0] = control_map[currKey];
		}
	    }
	  else if(keyMods & PS2KBD_ALT) /* ALT */
	    {
	      if(alt_map[currKey])
		{
		  currChars[0] = alt_map[currKey];
		}
	    }
	  else if(keyMods & PS2KBD_SHIFT) /* SHIFT */
	    {
	      if((ledStatus & PS2KBD_LED_CAPSLOCK) && (keycap[currKey]))
		{
		  currChars[0] = keymap[currKey];
		}
	      else
		{
		  currChars[0] = shiftkeymap[currKey];
		}
	    }
	  else /* Normal key */
	    {
	      if(keymap[keys[loopKey]])
		{
		  if((ledStatus & PS2KBD_LED_CAPSLOCK) && (keycap[currKey]))
		    {
		      currChars[0] = shiftkeymap[currKey];
		    }
		  else
		    {
		      currChars[0] = keymap[currKey];
		    }
		}
	    }
	}

      if((currChars[0] == PS2KBD_ESCAPE_KEY) && (currChars[1] != 0))
	{
	  if(lineEndP != (u32)(tempPos - 2))
	    {
	      lineBuffer[lineEndP++] = currChars[0];
	      lineEndP %= lineSize;
	      lineBuffer[lineEndP++] = currChars[1];
	      lineEndP %= lineSize;
	      byteCount += 2;
	    }
	  dev->repeatkeys[0] = currChars[0];
	  dev->repeatkeys[1] = currChars[1];
	}
      else if(currChars[0] != 0)
	{
	  lineBuffer[lineEndP++] = currChars[0];
	  lineEndP %= lineSize;
	  byteCount++;
	  dev->repeatkeys[0] = currChars[0];
	  dev->repeatkeys[1] = 0;
	}
    }

  if(byteCount > 0)
    {
      iop_sys_clock_t t;
      /* Set alarm to do repeat rate */
      //printf("repeatkeys %d %d\n", kbd_repeatkeys[0], kbd_repeatkeys[1]);
      USec2SysClock(PS2KBD_REPEATWAIT * 1000, &t);
      SetAlarm(&t, (void *)ps2kbd_repeathandler, dev);
    }

  for(loopKey = 0; loopKey < byteCount; loopKey++) /* Signal the sema to indicate data */
    {
      SignalSema(bufferSema);
    }

/*   lineBuffer[PS2KBD_DEFLINELEN - 1] = 0; */
/*   printf(lineBuffer); */
  //printf("lineStart %d, lineEnd %d\n", lineStartP, lineEndP);
}


void ps2kbd_getkeys_raw(u8 newKeyMods, u8 oldKeyMods, const u8 *new, const u8 *old)

{
  int loopKey;
  u8 currKey;
  u8 keyMods = newKeyMods ^ oldKeyMods;
  u8 keyModsMap = newKeyMods & keyMods;
  int tempPos = 0;
  int byteCount = 0;

  if(lineStartP < lineEndP)
    {
      tempPos = lineStartP + lineSize;
    }
  else
    {
      tempPos = lineStartP;
    }

  for(loopKey = 0; loopKey < 8; loopKey++)
    {
      int currMod = (1 << loopKey);
      if(keyMods & currMod)
	{
	  if(lineEndP == (u32)(tempPos - 2))
	    {
	      return;
	    }

	  currKey = keyModValue[loopKey];

	  if(keyModsMap & currMod) /* If key pressed */
	    {
	      lineBuffer[lineEndP++] = PS2KBD_RAWKEY_DOWN;
	      //printf("Key down\n");
	    }
	  else
	    {
	      lineBuffer[lineEndP++] = PS2KBD_RAWKEY_UP;
	      //printf("Key up\n");
	    }

	  lineEndP %= lineSize;
	  lineBuffer[lineEndP++] = currKey;
	  lineEndP %= lineSize;
	  byteCount += 2;
	  //printf("Key %d\n", currKey);
	}
    }

  for(loopKey = 0; loopKey < PS2KBD_MAXKEYS; loopKey++)
    {
      if(lineEndP == (u32)(tempPos - 2))
	{
	  return;
	}

      if(new[loopKey] != 0)
	{
	  lineBuffer[lineEndP++] = PS2KBD_RAWKEY_DOWN;
	  lineEndP %= lineSize;
	  lineBuffer[lineEndP++] = new[loopKey];
	  lineEndP %= lineSize;
	  byteCount += 2;
	  //printf("Key down\nKey %d\n", new[loopKey]);
	}

    }

  for(loopKey = 0; loopKey < PS2KBD_MAXKEYS; loopKey++)
    {
      if(lineEndP == (u32)(tempPos - 2))
	{
	  return;
	}

      if(old[loopKey] != 0)
	{
	  lineBuffer[lineEndP++] = PS2KBD_RAWKEY_UP;
	  lineEndP %= lineSize;
	  lineBuffer[lineEndP++] = old[loopKey];
	  lineEndP %= lineSize;
	  byteCount += 2;
	  //printf("Key up\nKey %d\n", old[loopKey]);
	}

    }

  for(loopKey = 0; loopKey < byteCount; loopKey++) /* Signal the sema for the number of bytes read */
    {
      SignalSema(bufferSema);
    }
}

void ps2kbd_data_recv(int resultCode, int bytes, void *arg)

{
  kbd_dev *dev;
  int phantom;
  int loop;

  if(resultCode != USB_RC_OK)
    {
      printf("PS2KEYBOARD: Data Recv set res %d, bytes %d, arg %p\n", resultCode, bytes, arg);
      return;
    }

  //printf("PS2KBD: Data Recv set res %d, bytes %d, arg %p\n", resultCode, bytes, arg);

  dev = (kbd_dev *) arg;
  if(dev == NULL)
    {
      printf("PS2KBD: dev == NULL\n");
      return;
    }

/*       printf("PS2KBD Modifiers %02X, Keys ", dev->data.mod_keys); */
/*       for(loop = 0; loop < PS2KBD_MAXKEYS; loop++) */
/* 	{ */
/* 	  printf("%02X ", dev->data.keycodes[loop]); */
/* 	} */
/*       printf("\n"); */

  CancelAlarm((void *)ps2kbd_repeathandler, dev); /* Make sure repeat alarm is cancelled */

  /* Check for phantom states */
  phantom = 1;
  for(loop = 0; loop < PS2KBD_MAXKEYS; loop++)
    {
      if(dev->data.keycodes[loop] != 1)
	{
	  phantom = 0;
	  break;
	}
    }

  if(!phantom) /* If not in a phantom state */
    {
      u8 uniqueKeys[PS2KBD_MAXKEYS];
      u8 missingKeys[PS2KBD_MAXKEYS];

      memset(uniqueKeys, 0, PS2KBD_MAXKEYS);
      memset(missingKeys, 0, PS2KBD_MAXKEYS);
      ps2kbd_build_uniquekeys(uniqueKeys, dev->data.keycodes, dev->oldData.keycodes);
      ps2kbd_build_uniquekeys(missingKeys, dev->oldData.keycodes, dev->data.keycodes);
      /* Build new and missing key lists */

/*       printf("Unique keys : "); */
/*       for(loopKey = 0; loopKey < PS2KBD_MAXKEYS; loopKey++) */
/* 	{ */
/* 	  printf("%02X ", uniqueKeys[loopKey]); */
/* 	} */
/*       printf("\n"); */

/*       printf("Missing keys : "); */
/*       for(loopKey = 0; loopKey < PS2KBD_MAXKEYS; loopKey++) */
/* 	{ */
/* 	  printf("%02X ", missingKeys[loopKey]); */
/* 	} */
/*       printf("\n"); */

      if(kbd_readmode == PS2KBD_READMODE_NORMAL)
	{
    int loopKey;
	  u8 ledStatus;

	  ledStatus = dev->ledStatus;
	  //printf("ledStatus %02X\n", ledStatus);

	  for(loopKey = 0; loopKey < PS2KBD_MAXKEYS; loopKey++) /* Process key codes */
	    {
	      switch(uniqueKeys[loopKey])
		{
		case USB_KEYB_NUMLOCK :
		  ledStatus ^= PS2KBD_LED_NUMLOCK;
		  uniqueKeys[loopKey] = 0;
		  break;
		case USB_KEYB_CAPSLOCK :
		  ledStatus ^= PS2KBD_LED_CAPSLOCK;
		  uniqueKeys[loopKey] = 0;
		  break;
		case USB_KEYB_SCRLOCK :
		  ledStatus ^= PS2KBD_LED_SCRLOCK;
		  uniqueKeys[loopKey] = 0;
		  break;
		}
	    }

	  if(ledStatus != dev->ledStatus)
	    {
	      dev->ledStatus = ledStatus & PS2KBD_LED_MASK;
	      //printf("LEDS %02X\n", dev->ledStatus);
	      /* Call Set LEDS */
	      sceUsbdControlTransfer(dev->configEndp, 0x21, USB_REQ_SET_REPORT, 0x200,
				 dev->interfaceNo, 1, &dev->ledStatus, ps2kbd_led_set, arg);
	    }

	  WaitSema(lineSema); /* Make sure no other thread is going to manipulate the buffer */
	  ps2kbd_getkeys(dev->data.mod_keys, dev->ledStatus, uniqueKeys, dev); /* read in remaining keys */
	  SignalSema(lineSema);
	}
      else /* RAW Mode */
	{
	  WaitSema(lineSema);
	  ps2kbd_getkeys_raw(dev->data.mod_keys, dev->oldData.mod_keys, uniqueKeys, missingKeys);
	  SignalSema(lineSema);
	}

      memcpy(&dev->oldData, &dev->data, sizeof(kbd_data_recv));
    }

  sceUsbdInterruptTransfer(dev->dataEndp, &dev->data, dev->packetSize, ps2kbd_data_recv, arg);
}

void flushbuffer()

{
  iop_sema_t s;

  lineStartP = 0;
  lineEndP = 0;
  memset(lineBuffer, 0, lineSize);

  DeleteSema(bufferSema);
  s.initial = 0;
  s.max = lineSize;
  s.option = 0;
  s.attr = 0;
  bufferSema = CreateSema(&s); /* Create a sema to maintain status of readable data */

  if(bufferSema <= 0)
    {
      printf("PS2KBD: Error creating buffer sema\n");
    }
}

void ps2kbd_ioctl_setreadmode(u32 readmode)

{

  if(readmode == (u32)kbd_readmode) return;

  if((readmode == PS2KBD_READMODE_NORMAL) || (readmode == PS2KBD_READMODE_RAW))
    {
      int devLoop;

      /* Reset line buffer */
      //printf("ioctl_setreadmode %d\n", readmode);
      for(devLoop = 0; devLoop < PS2KBD_MAXDEV; devLoop++)
	{
	  CancelAlarm((void *)ps2kbd_repeathandler, devices[devLoop]);
	}

      WaitSema(lineSema);
      kbd_readmode = readmode;
      flushbuffer();
      SignalSema(lineSema);
    }
}

void ps2kbd_ioctl_setkeymap(kbd_keymap *keymaps)

{
  //printf("ioctl_setkeymap %p\n", keymaps);
  WaitSema(lineSema);   /* Lock the input so you dont end up with weird results */
  memcpy(keymap, keymaps->keymap, PS2KBD_KEYMAP_SIZE);
  memcpy(shiftkeymap, keymaps->shiftkeymap, PS2KBD_KEYMAP_SIZE);
  memcpy(keycap, keymaps->keycap, PS2KBD_KEYMAP_SIZE);
  SignalSema(lineSema);
}

void ps2kbd_ioctl_setctrlmap(u8 *ctrlmap)

{
  //printf("ioctl_setctrlmap %p\n", ctrlmap);
  WaitSema(lineSema);
  memcpy(control_map, ctrlmap, PS2KBD_KEYMAP_SIZE);
  SignalSema(lineSema);
}

void ps2kbd_ioctl_setaltmap(u8 *altmap)

{
  //printf("ioctl_setaltmap %p\n", altmap);
  WaitSema(lineSema);
  memcpy(alt_map, altmap, PS2KBD_KEYMAP_SIZE);
  SignalSema(lineSema);
}

void ps2kbd_ioctl_setspecialmap(u8 *special)

{
  //printf("ioctl_setspecialmap %p\n", special);
  WaitSema(lineSema);
  memcpy(special_keys, special, PS2KBD_KEYMAP_SIZE);
  SignalSema(lineSema);
}

void ps2kbd_ioctl_resetkeymap()
     /* Reset keymap to default US variety */

{
  //printf("ioctl_resetkeymap()\n");
  WaitSema(lineSema);
  memcpy(keymap, us_keymap, PS2KBD_KEYMAP_SIZE);
  memcpy(shiftkeymap, us_shiftkeymap, PS2KBD_KEYMAP_SIZE);
  memcpy(keycap, us_keycap, PS2KBD_KEYMAP_SIZE);
  memcpy(special_keys, us_special_keys, PS2KBD_KEYMAP_SIZE);
  memcpy(control_map, us_control_map, PS2KBD_KEYMAP_SIZE);
  memcpy(alt_map, us_alt_map, PS2KBD_KEYMAP_SIZE);
  SignalSema(lineSema);
}

void ps2kbd_ioctl_flushbuffer()
     /* Flush the internal buffer */

{
  //printf("ioctl_flushbuffer()\n");
  WaitSema(lineSema);
  flushbuffer();
  SignalSema(lineSema);
}

void ps2kbd_ioctl_setleds(u8 ledStatus)

{
  int devLoop;
  kbd_dev *dev;

  //printf("ioctl_setleds %d\n", ledStatus);
  ledStatus &= PS2KBD_LED_MASK;
  for(devLoop = 0; devLoop < PS2KBD_MAXDEV; devLoop++)
    {
      dev = devices[devLoop];
      if(dev)
	{
	  if(ledStatus != dev->ledStatus)
	    {
	      dev->ledStatus = ledStatus & PS2KBD_LED_MASK;
	      sceUsbdControlTransfer(dev->configEndp, 0x21, USB_REQ_SET_REPORT, 0x200,
				 dev->interfaceNo, 1, &dev->ledStatus, ps2kbd_led_set, dev);
	    }
	}
    }
}

void ps2kbd_ioctl_setblockmode(u32 blockmode)

{
  if((blockmode == PS2KBD_BLOCKING) || (blockmode == PS2KBD_NONBLOCKING))
    {
      kbd_blocking = blockmode;
      //printf("ioctl_setblockmode %d\n", blockmode);
    }
}

void ps2kbd_ioctl_setrepeatrate(u32 rate)

{
  kbd_repeatrate = rate;
}

int fio_dummy()
{
  //printf("fio_dummy()\n");
  return -5;
}

int fio_init(iop_device_t *driver)
{
  (void)driver;

  //printf("fio_init()\n");
  return 0;
}

int fio_format(iop_file_t *f)
{
  (void)f;

  //printf("fio_format()\n");
  return 0;
}

int fio_open(iop_file_t *f, const char *name, int mode)
{
  (void)f;
  (void)mode;

  //printf("fio_open() %s %d\n", name, mode);
  if(strcmp(name, PS2KBD_KBDFILE)) /* If not the keyboard file */
    {
      return -1;
    }

  return 0;
}

int fio_read(iop_file_t *f, void *buf, int size)

{
  int count = 0;
  char *data = (char *) buf;
  int ret;

  (void)f;

  //printf("fio_read() %p %d\n", buf, size);

  if(kbd_readmode == PS2KBD_READMODE_RAW)
    {
      size &= ~1; /* Ensure size of a multiple of 2 */
    }

  ret = PollSema(bufferSema);
  if(ret < 0)
    {
      if((ret == SEMA_ZERO) && (kbd_blocking == PS2KBD_BLOCKING))
	{
	  //printf("Blocking\n");
	  ret = WaitSema(bufferSema);
	  if(ret < 0) /* Means either an error or the sema was deleted from under us */
	    {
	      return 0;
	    }
	}
      else
	{
	  return 0;
	}
    }

  SignalSema(bufferSema);
  ret = WaitSema(lineSema);
  if(ret < 0) return 0;

  while((count < size) && (lineStartP != lineEndP))
    {
      data[count] = lineBuffer[lineStartP++];
      lineStartP %= lineSize;
      count++;
      PollSema(bufferSema); /* Take off one count from the sema */
    }

  SignalSema(lineSema);
/*   //printf("read %d bytes\n", count); */
/*   { */
/*     struct t_sema_status s; */

/*     ReferSemaStatus(bufferSema, &s); */
/*     //printf("Sema count %d\n", s.curr_count); */
/*   } */

  return count;
}

int fio_ioctl(iop_file_t *f, int cmd, void *param)

{
  (void)f;

  //printf("fio_ioctl() %ld %d\n", cmd, *((u32 *) param));
  switch(cmd)
    {
    case PS2KBD_IOCTL_SETREADMODE: ps2kbd_ioctl_setreadmode(*((u32 *) param));
      break;
    case PS2KBD_IOCTL_SETKEYMAP: ps2kbd_ioctl_setkeymap((kbd_keymap *) param);
      break;
    case PS2KBD_IOCTL_SETALTMAP: ps2kbd_ioctl_setaltmap((u8 *) param);
      break;
    case PS2KBD_IOCTL_SETCTRLMAP: ps2kbd_ioctl_setctrlmap((u8 *) param);
      break;
    case PS2KBD_IOCTL_SETSPECIALMAP: ps2kbd_ioctl_setspecialmap((u8 *) param);
      break;
    case PS2KBD_IOCTL_FLUSHBUFFER: ps2kbd_ioctl_flushbuffer();
      break;
    case PS2KBD_IOCTL_SETLEDS: ps2kbd_ioctl_setleds(*(u8*) param);
      break;
    case PS2KBD_IOCTL_SETBLOCKMODE: ps2kbd_ioctl_setblockmode(*(u32 *) param);
      break;
    case PS2KBD_IOCTL_RESETKEYMAP: ps2kbd_ioctl_resetkeymap();
      break;
    case PS2KBD_IOCTL_SETREPEATRATE: ps2kbd_ioctl_setrepeatrate(*(u32 *) param);
      break;
    default : return -1;
    }

  return 0;
}

int fio_close(iop_file_t *f)

{
  (void)f;

  //printf("fio_close()\n");
  return 0;
}

static iop_device_ops_t fio_ops =

  {
    &fio_init,
    (void *)&fio_dummy,
    &fio_format,
    &fio_open,
    &fio_close,
    &fio_read,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    &fio_ioctl,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
    (void *)&fio_dummy,
  };

static iop_device_t kbd_filedrv = {
  PS2KBD_FSNAME,
  IOP_DT_CHAR,
  0,
  "USB Keyboard FIO driver",
  &fio_ops,
};


int init_fio()

{
  return AddDrv(&kbd_filedrv);
}

void repeat_thread(void *arg)

{
  u32 eventmask;
  int devLoop;

  (void)arg;

  for(;;)
    {
      WaitEventFlag(eventid, 0xFFFFFFFF, 0x01 | 0x10, &eventmask);
      //printf("Recieved event %08X\n", eventmask);
      for(devLoop = 0; devLoop < PS2KBD_MAXDEV; devLoop++)
	{
	  if((eventmask & (1 << devLoop)) && (devices[devLoop]))
	    {
	      int tempPos = 0;

	      WaitSema(lineSema);
	      if(lineStartP < lineEndP)
		{
		  tempPos = lineStartP + lineSize;
		}
	      else
		{
		  tempPos = lineStartP;
		}

	      if((devices[devLoop]->repeatkeys[0]) && (devices[devLoop]->repeatkeys[1]))
		{
		  if(lineEndP != (u32)(tempPos - 2))
		    {
		      lineBuffer[lineEndP++] = devices[devLoop]->repeatkeys[0];
		      lineEndP %= lineSize;
		      lineBuffer[lineEndP++] = devices[devLoop]->repeatkeys[1];
		      lineEndP %= lineSize;
		      SignalSema(bufferSema);
		      SignalSema(bufferSema);
		    }
		}
	      else if(devices[devLoop]->repeatkeys[0])
		{
		  if(lineEndP != (u32)(tempPos - 1))
		    {
		      lineBuffer[lineEndP++] = devices[devLoop]->repeatkeys[0];
		      lineEndP %= lineSize;
		      SignalSema(bufferSema);
		    }
		}

	      SignalSema(lineSema);
	    }
	}
    }
}

int init_repeatthread()
     /* Creates a thread to handle key repeats */

{
  iop_thread_t param;
  iop_event_t event;

  event.attr = 0;
  event.option = 0;
  event.bits = 0;
  eventid = CreateEventFlag(&event);

  param.attr         = TH_C;
  param.thread    = repeat_thread;
  param.priority     = 40;
  param.stacksize    = 0x800;
  param.option       = 0;

  repeat_tid = CreateThread(&param);
  if (repeat_tid > 0) {
    StartThread(repeat_tid, 0);
    return 0;
  }
  else
    {
      return 1;
    }
}

int ps2kbd_init()

{
  int ret;
  iop_sema_t s;

  s.initial = 1;
  s.max = 1;
  s.option = 0;
  s.attr = 0;
  lineSema = CreateSema(&s);
  if(lineSema <= 0)
    {
      printf("PS2KBD: Error creating sema\n");
      return 1;
    }

  s.initial = 0;
  s.max = PS2KBD_DEFLINELEN;
  s.option = 0;
  s.attr = 0;
  bufferSema = CreateSema(&s); /* Create a sema to maintain status of readable data */
  if(bufferSema <= 0)
    {
      printf("PS2KBD: Error creating buffer sema\n");
      return 1;
    }

  lineBuffer = (u8 *) AllocSysMemory(0, PS2KBD_DEFLINELEN, NULL);
  if(lineBuffer == NULL)
    {
      printf("PS2KBD: Error allocating line buffer\n");
      return 1;
    }
  lineStartP = 0;
  lineEndP = 0;
  lineSize = PS2KBD_DEFLINELEN;
  memset(lineBuffer, 0, PS2KBD_DEFLINELEN);

  memset(devices, 0, sizeof(kbd_dev *) * PS2KBD_MAXDEV);
  dev_count = 0;
  kbd_blocking = PS2KBD_NONBLOCKING;
  kbd_readmode = PS2KBD_READMODE_NORMAL;
  kbd_repeatrate = PS2KBD_DEFREPEATRATE;
  memcpy(keymap, us_keymap, PS2KBD_KEYMAP_SIZE);
  memcpy(shiftkeymap, us_shiftkeymap, PS2KBD_KEYMAP_SIZE);
  memcpy(keycap, us_keycap, PS2KBD_KEYMAP_SIZE);
  memcpy(special_keys, us_special_keys, PS2KBD_KEYMAP_SIZE);
  memcpy(control_map, us_control_map, PS2KBD_KEYMAP_SIZE);
  memcpy(alt_map, us_alt_map, PS2KBD_KEYMAP_SIZE);

  ret = init_fio();
  if(ret < 0)
    {
      printf("PS2KBD: Error adding ioman driver\n");
      return 1;
    }
  //printf("ps2kbd AddDrv [%d]\n", ret);
  init_repeatthread();

  ret = sceUsbdRegisterLdd(&kbd_driver);
  if(ret != USB_RC_OK)
    {
      printf("PS2KBD: Error registering USB devices\n");
      return 1;
    }

  //printf("sceUsbdRegisterLdd %d\n", ret);

  return 0;
}
