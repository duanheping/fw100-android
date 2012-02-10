
> Let me explain way that I run GPS on android-x86.
>
> 1. First of all, you have to search and download, 'gps_freerunner.c'
>
> 2. Move 'gps_freerunner.c' to 'hardware/libhardware_legacy/gps/'
> folder.
>
> 3. Modify 'hardware/libhardware_legacy/gps/Android.mk' like below
> -------------------------------------------------------
> # Use hardware GPS implementation if available.
> #
>   LOCAL_CFLAGS           += -DHAVE_GPS_HARDWARE
>   # LOCAL_SHARED_LIBRARIES += $(BOARD_GPS_LIBRARIES)
>   LOCAL_SRC_FILES += gps/gps_freerunner.c
> -------------------------------------------------------
>
> 4. Modify 'hardware/libhardware_legacy/gps/gps_freerunner.c'like below
> -------------------------------------------------------
> @@ -14,7 +14,7 @@
>  #include <cutils/log.h>
>  #include <cutils/sockets.h>
>  #include <cutils/properties.h>
> -#include <hardware/gps.h>
> +#include <hardware_legacy/gps.h>
>
>  #define  GPS_DEBUG  0
>
> @@ -1098,10 +1098,11 @@
>          return;
>      }
>
> -    if ( snprintf(device, sizeof(device), "/dev/%s", prop) >=
> (int)sizeof(device) ) {
> -        LOGE("gps serial device name too long: '%s'", prop);
> -        return;
> -    }
> +    //if ( snprintf(device, sizeof(device), "/dev/%s", prop) >=
> (int)sizeof(device) ) {
> +    //    LOGE("gps serial device name too long: '%s'", prop);
> +    //    return;
> +    //}
> +    snprintf(device, sizeof(device), "/dev/%s",prop);
>
>      do {
>          state->fd = open( device, O_RDWR );
> @@ -1296,7 +1297,7 @@
>      int fd;
>      char cmd = '0';
>      int ret;
> -
> +#if 0   // disble
>      if (property_get("gps.power_on",prop,GPS_POWER_IF) == 0) {
>          LOGE("no gps power interface name");
>          return;
> @@ -1324,7 +1325,7 @@
>      close(fd);
>
>      DFR("gps power state = %c", cmd);
> -
> +#endif
>      return;
>
>  }
> -------------------------------------------------------
>
> 5. For your USB or Serial GPS Device, your kernel must recognize !!!
> For me, I use USB-GPS(PL2303), and kernel recognize by pl2303.ko
> kernel module(CONFIG_USB_SERIAL_PL2303).
> You simply check your device driver in ubuntu. However, default
> android-x86 kernel does not include
> pl2303 module, and there is not much driver for usb-to-serial module.
> So, you have to customize kernel config and build your kernel.
> After recognizing GPS device, you can check your device in '/dev/',
> like '/dev/ttyUSBx' for USB2Serial.
>
> You must add property for GPS device like below.(for example, assume
> ttyUSB0)
>
> > setprop ro.kernel.android.gps ttyUSB0
>
> If you want to view demo, you can see GPS usage demo in
> 'www.androbox.co.kr> screenshot'.
>
> Regards
>
> On 10월28일, 오전11시27분, Bamm-Bamm <neocs...@gmail.com> wrote:> If you get that working, I'd
be willing to purchase the software from
> > you.
>
> > On Oct 7, 6:54 pm, BenjaminWebb <benjaminrw...@...> wrote:
>
> > > No that service looks easy to program I just need the drivers and gpsd
> > > to load so I can convert from gpsd to the location provider unless you
> > > guys are looking to reinvent the wheel.
>
> > > On Oct 7, 5:58 am, kwok <kwok.w...@...> wrote:
>
> > > > I believe you have to implement your own  LocationProvider to process
> > > > the NMEA yourGPSdevice and add this LocationProvider to the Android-
> > > > x86,
> > > > seehttp://developer.android.com/reference/android/location/LocationProvi...
>
> > > > Then, you can use anyGPSapp from the Android market ..
>
> > > > Obviously, the hard part is to implement and add such LocationProvider
> > > > service  to the Android-x86.
>
> > > > On Oct 7, 2:22 am, BenjaminWebb <benjaminrw...@...> wrote:
>
> > > > > I was wondering what the status is of running agpson android x86.  i
> > > > > have a surfstar II usb one currently supported under Ubuntu but I have
> > > > > yet to findgpssoftware with descent routing and traffic etc.  I see
> > > > > android as the perfect operating system for a car tablet or computer.
> > > > > Just waiting for the support to get there.

