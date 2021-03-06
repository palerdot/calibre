/*    Copyright (C) 2008 Kovid Goyal kovid@kovidgoyal.net
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License along
 *    with this program; if not, write to the Free Software Foundation, Inc.,
 *    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * Python extension to scan the system for USB devices on OS X machines.
 * To use
 * >>> import usbobserver
 * >>> usbobserver.get_devices()
 */


#include <Python.h>

#include <stdio.h>

#include <CoreFoundation/CFNumber.h>
#include <CoreServices/CoreServices.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/IOBSD.h>
#include <IOKit/usb/USBSpec.h>

#include <mach/mach.h>
#include <sys/param.h>
#include <paths.h>
#include <sys/ucred.h>
#include <sys/mount.h>

#ifndef kUSBVendorString
#define kUSBVendorString "USB Vendor Name"
#endif

#ifndef kUSBProductString
#define kUSBProductString "USB Product Name"
#endif

#ifndef kUSBSerialNumberString
#define kUSBSerialNumberString "USB Serial Number"
#endif

#define NUKE(x) Py_XDECREF(x); x = NULL;

/* This function only works on 10.5 and later. Pass in a unicode object as path */
static PyObject* usbobserver_send2trash(PyObject *self, PyObject *args)
{
    UInt8 *utf8_chars;
    FSRef fp;
    OSStatus op_result;

    if (!PyArg_ParseTuple(args, "es", "utf-8", &utf8_chars)) {
        return NULL;
    }

    FSPathMakeRefWithOptions(utf8_chars, kFSPathMakeRefDoNotFollowLeafSymlink, &fp, NULL);
    op_result = FSMoveObjectToTrashSync(&fp, NULL, kFSFileOperationDefaultOptions);
    PyMem_Free(utf8_chars);
    if (op_result != noErr) {
        PyErr_SetString(PyExc_OSError, GetMacOSStatusCommentString(op_result));
        return NULL;
    }
    Py_RETURN_NONE;
}


static PyObject*
usbobserver_get_iokit_string_property(io_service_t dev, CFStringRef prop) {
    CFTypeRef PropRef;
    char buf[500];

    PropRef = IORegistryEntryCreateCFProperty(dev, prop, kCFAllocatorDefault, 0);
    if (PropRef) {
        if(!CFStringGetCString(PropRef, buf, 500, kCFStringEncodingUTF8)) buf[0] = '\0';
        CFRelease(PropRef);
    } else buf[0] = '\0';

    return PyUnicode_DecodeUTF8(buf, strlen(buf), "replace");
}

static PyObject*
usbobserver_get_iokit_number_property(io_service_t dev, CFStringRef prop) {
    CFTypeRef PropRef;
    long val = 0;

    PropRef = IORegistryEntryCreateCFProperty(dev, prop, kCFAllocatorDefault, 0);
    if (PropRef) {
        CFNumberGetValue((CFNumberRef)PropRef, kCFNumberLongType, &val);
        CFRelease(PropRef);
    } 

    return PyLong_FromLong(val);
}


static PyObject *
usbobserver_get_usb_devices(PyObject *self, PyObject *args) {
  
    CFMutableDictionaryRef matchingDict;
    kern_return_t kr;
    PyObject *devices, *device;
    io_service_t usbDevice;
    PyObject *vendor, *product, *bcd;
    PyObject *manufacturer, *productn, *serial;



    //Set up matching dictionary for class IOUSBDevice and its subclasses
    matchingDict = IOServiceMatching(kIOUSBDeviceClassName);
    if (!matchingDict) {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't create a USB matching dictionary");
        return NULL;
    }

    io_iterator_t iter;
    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter);
    if (KERN_SUCCESS != kr) {
            printf("IOServiceGetMatchingServices returned 0x%08x\n", kr);
            PyErr_SetString(PyExc_RuntimeError, "Could not run IO Matching");
            return NULL;
    }

    devices = PyList_New(0);
    if (devices == NULL) {
        PyErr_NoMemory();
        return NULL;
    }


    while ((usbDevice = IOIteratorNext(iter))) {

        vendor = usbobserver_get_iokit_number_property(usbDevice, CFSTR(kUSBVendorID));
        product = usbobserver_get_iokit_number_property(usbDevice, CFSTR(kUSBProductID));
        bcd = usbobserver_get_iokit_number_property(usbDevice, CFSTR(kUSBDeviceReleaseNumber));
        manufacturer = usbobserver_get_iokit_string_property(usbDevice, CFSTR(kUSBVendorString));
        productn = usbobserver_get_iokit_string_property(usbDevice, CFSTR(kUSBProductString));
        serial = usbobserver_get_iokit_string_property(usbDevice, CFSTR(kUSBSerialNumberString));
        if (usbDevice) IOObjectRelease(usbDevice);

        if (vendor != NULL && product != NULL && bcd != NULL) {

            if (manufacturer == NULL) { manufacturer = Py_None; Py_INCREF(Py_None); }
            if (productn == NULL) { productn = Py_None; Py_INCREF(Py_None); }
            if (serial == NULL) { serial = Py_None; Py_INCREF(Py_None); }

            device = Py_BuildValue("(OOOOOO)", vendor, product, bcd, manufacturer, productn, serial);
            if (device != NULL) {
                PyList_Append(devices, device);
                Py_DECREF(device);
            }
        }

        NUKE(vendor); NUKE(product); NUKE(bcd); NUKE(manufacturer);
        NUKE(productn); NUKE(serial);
    }
    
    if (iter) IOObjectRelease(iter);

    return devices;
}

static PyObject*
usbobserver_get_bsd_path(io_object_t dev) {
    char cpath[ MAXPATHLEN ];
    CFTypeRef PropRef;
    size_t dev_path_length;

    cpath[0] = '\0';
    PropRef = IORegistryEntryCreateCFProperty(dev, CFSTR(kIOBSDNameKey), kCFAllocatorDefault, 0);
    if (!PropRef) return NULL;
    strcpy(cpath, _PATH_DEV);
    dev_path_length = strlen(cpath);

    if (!CFStringGetCString(PropRef,
                        cpath + dev_path_length,
                        MAXPATHLEN - dev_path_length, 
                        kCFStringEncodingUTF8)) return NULL;

    return PyUnicode_DecodeUTF8(cpath, strlen(cpath), "replace");

}

static PyObject* 
usbobserver_find_prop(io_registry_entry_t e, CFStringRef key, int is_string )
{
    char buf[500]; long val = 0;
    PyObject *ans;
    IOOptionBits bits = kIORegistryIterateRecursively | kIORegistryIterateParents;
    CFTypeRef PropRef = IORegistryEntrySearchCFProperty( e, kIOServicePlane, key, NULL, bits );

    if (!PropRef) return NULL;
    buf[0] = '\0';

    if(is_string) {
        CFStringGetCString(PropRef, buf, 500, kCFStringEncodingUTF8);
        ans = PyUnicode_DecodeUTF8(buf, strlen(buf), "replace");
    } else {
        CFNumberGetValue((CFNumberRef)PropRef, kCFNumberLongType, &val);
        ans = PyLong_FromLong(val);
    }

    CFRelease(PropRef);
    return ans;
} 

static PyObject*
usbobserver_get_usb_drives(PyObject *self, PyObject *args) {
    CFMutableDictionaryRef matchingDict;
    kern_return_t kr = KERN_FAILURE;
    io_iterator_t iter;
    io_object_t        next;
    PyObject *ans = NULL, *bsd_path = NULL, *t = NULL, *vid, *pid, *bcd, *manufacturer, *product, *serial;

    //Set up matching dictionary for class IOMedia and its subclasses
    matchingDict = IOServiceMatching(kIOMediaClass);
    if (!matchingDict) {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't create a Media matching dictionary");
        return NULL;
    }
    // Only want writable and ejectable leaf nodes
    CFDictionarySetValue(matchingDict, CFSTR(kIOMediaWritableKey), kCFBooleanTrue);
    CFDictionarySetValue(matchingDict, CFSTR(kIOMediaLeafKey), kCFBooleanTrue);
    CFDictionarySetValue(matchingDict, CFSTR(kIOMediaEjectableKey), kCFBooleanTrue);

    ans = PyList_New(0);
    if (ans == NULL) return PyErr_NoMemory();

    kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iter);
    if (KERN_SUCCESS != kr) {
            printf("IOServiceGetMatchingServices returned 0x%08x\n", kr);
            PyErr_SetString(PyExc_RuntimeError, "Could not run IO Matching");
            return NULL;
    }

    while ((next = IOIteratorNext(iter))) {
        bsd_path = usbobserver_get_bsd_path(next);
        vid = usbobserver_find_prop(next, CFSTR(kUSBVendorID), 0);
        pid = usbobserver_find_prop(next, CFSTR(kUSBProductID), 0);
        bcd = usbobserver_find_prop(next, CFSTR(kUSBDeviceReleaseNumber), 0);
        manufacturer = usbobserver_find_prop(next, CFSTR(kUSBVendorString), 1);
        product = usbobserver_find_prop(next, CFSTR(kUSBProductString), 1);
        serial = usbobserver_find_prop(next, CFSTR(kUSBSerialNumberString), 1);

        IOObjectRelease(next);

        if (bsd_path != NULL && vid != NULL && pid != NULL && bcd != NULL) {
            if (manufacturer == NULL) { manufacturer = Py_None; Py_INCREF(Py_None); }
            if (product == NULL) { product = Py_None; Py_INCREF(Py_None); }
            if (serial == NULL) { serial = Py_None; Py_INCREF(Py_None); }

            t = Py_BuildValue("(OOOOOOO)", bsd_path, vid, pid, bcd, manufacturer, product, serial);
            if (t != NULL) {
                PyList_Append(ans, t);
                Py_DECREF(t); t = NULL;
            }
        }
        NUKE(bsd_path); NUKE(vid); NUKE(pid); NUKE(bcd);
        NUKE(manufacturer); NUKE(product); NUKE(serial);
    }

    if (iter) IOObjectRelease(iter);

    return ans;
}

static PyObject*
usbobserver_get_mounted_filesystems(PyObject *self, PyObject *args) {
    struct statfs *buf, t;
    int num, i;
    PyObject *ans, *key, *val;

    num = getfsstat(NULL, 0, MNT_NOWAIT);
    if (num == -1) {
        PyErr_SetString(PyExc_RuntimeError, "Initial call to getfsstat failed");
        return NULL;
    }
    ans = PyDict_New();
    if (ans == NULL) return PyErr_NoMemory();

    buf = (struct statfs*)calloc(num, sizeof(struct statfs));
    if (buf == NULL) return PyErr_NoMemory();

    num = getfsstat(buf, num*sizeof(struct statfs), MNT_NOWAIT);
    if (num == -1) {
        PyErr_SetString(PyExc_RuntimeError, "Call to getfsstat failed");
        return NULL;
    }

    for (i = 0 ; i < num; i++) {
        t = buf[i];
        key = PyBytes_FromString(t.f_mntfromname);
        val = PyBytes_FromString(t.f_mntonname);
        if (key != NULL && val != NULL) {
            PyDict_SetItem(ans, key, val);
        }
        NUKE(key); NUKE(val);
    }

    free(buf);

    return ans;

}

static PyObject*
usbobserver_user_locale(PyObject *self, PyObject *args) {
    	CFStringRef id = NULL;
        CFLocaleRef loc = NULL;
        char buf[512] = {0};
        PyObject *ans = NULL;
        int ok = 0;

        loc = CFLocaleCopyCurrent();
        if (loc) {
            id = CFLocaleGetIdentifier(loc);
            if (id && CFStringGetCString(id, buf, 512, kCFStringEncodingUTF8)) {
                ok = 1;
                ans = PyUnicode_FromString(buf);
            }
        }

        if (loc) CFRelease(loc);
        if (ok) return ans;
        Py_RETURN_NONE;
}

static PyObject*
usbobserver_date_fmt(PyObject *self, PyObject *args) {
    	CFStringRef fmt = NULL;
        CFLocaleRef loc = NULL;
        CFDateFormatterRef formatter = NULL;
        char buf[512] = {0};
        PyObject *ans = NULL;
        int ok = 0;

        loc = CFLocaleCopyCurrent();
        if (loc) {
            formatter = CFDateFormatterCreate(kCFAllocatorDefault, loc, kCFDateFormatterShortStyle, kCFDateFormatterNoStyle);
            if (formatter) {
                fmt = CFDateFormatterGetFormat(formatter);
                if (fmt && CFStringGetCString(fmt, buf, 512, kCFStringEncodingUTF8)) {
                    ok = 1;
                    ans = PyUnicode_FromString(buf);
                }
            }
        }
        if (formatter) CFRelease(formatter);
        if (loc) CFRelease(loc);
        if (ok) return ans;
        Py_RETURN_NONE;
}


static PyMethodDef usbobserver_methods[] = {
    {"get_usb_devices", usbobserver_get_usb_devices, METH_VARARGS, 
     "Get list of connected USB devices. Returns a list of tuples. Each tuple is of the form (vendor_id, product_id, bcd, manufacturer, product, serial number)."
    },
    {"get_usb_drives", usbobserver_get_usb_drives, METH_VARARGS, 
     "Get list of mounted drives. Returns a list of tuples, each of the form (name, bsd_path)."
    },
    {"get_mounted_filesystems", usbobserver_get_mounted_filesystems, METH_VARARGS, 
     "Get mapping of mounted filesystems. Mapping is from BSD name to mount point."
    },
    {"send2trash", usbobserver_send2trash, METH_VARARGS, 
     "send2trash(unicode object) -> Send specified file/dir to trash"
    },
    {"user_locale", usbobserver_user_locale, METH_VARARGS, 
     "user_locale() -> The name of the current user's locale or None if an error occurred"
    },
    {"date_format", usbobserver_date_fmt, METH_VARARGS, 
     "date_format() -> The (short) date format used by the user's current locale"
    },



    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initusbobserver(void) {
    (void) Py_InitModule("usbobserver", usbobserver_methods);
}
