#ifndef STUB_CF_H
#define STUB_CF_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned char UInt8;
typedef unsigned char Boolean;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef const void *CFTypeRef;
typedef const void *CFStringRef;
typedef const void *CFURLRef;
typedef const void *CFBundleRef;
typedef const void *CFDictionaryRef;
typedef const void *CFArrayRef;
typedef const void *CFNumberRef;
typedef const void *CFBooleanRef;
typedef const void *CFAllocatorRef;
typedef struct { CFIndex version; void *retain; void *release; void *copyDescription; } CFDictionaryKeyCallBacks;
typedef CFDictionaryKeyCallBacks CFDictionaryValueCallBacks;
typedef CFDictionaryKeyCallBacks CFArrayCallBacks;
typedef CFIndex CFNumberType;
#define kCFNumberIntType 9
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFBooleanRef kCFBooleanTrue;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
#define kCFStringEncodingUTF8 0x08000100
extern CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef alloc, const UInt8 *path, CFIndex pathLen, bool isDirectory);
extern CFBundleRef CFBundleCreate(CFAllocatorRef alloc, CFURLRef url);
extern CFStringRef CFBundleGetIdentifier(CFBundleRef bundle);
extern bool CFStringGetCString(CFStringRef str, char *buffer, CFIndex bufferSize, unsigned encoding);
extern CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cStr, unsigned encoding);
extern CFDictionaryRef CFDictionaryCreate(CFAllocatorRef alloc, const void **keys, const void **values, CFIndex numValues, const CFDictionaryKeyCallBacks *keyCallBacks, const CFDictionaryValueCallBacks *valueCallBacks);
extern const void *CFDictionaryGetValue(CFDictionaryRef theDict, const void *key);
extern CFArrayRef CFArrayCreate(CFAllocatorRef alloc, const void **values, CFIndex numValues, const CFArrayCallBacks *callBacks);
extern CFTypeID CFGetTypeID(CFTypeRef cf);
extern CFTypeID CFStringGetTypeID(void);
extern CFTypeID CFDictionaryGetTypeID(void);
extern CFTypeID CFNumberGetTypeID(void);
extern CFTypeID CFBooleanGetTypeID(void);
extern Boolean CFBooleanGetValue(CFBooleanRef boolean);
extern Boolean CFNumberGetValue(CFNumberRef number, CFNumberType theType, void *valuePtr);
extern void CFRelease(const void *cf);
#endif
