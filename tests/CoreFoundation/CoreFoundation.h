#ifndef STUB_CF_H
#define STUB_CF_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef unsigned char UInt8;
typedef long CFIndex;
typedef const void *CFStringRef;
typedef const void *CFURLRef;
typedef const void *CFBundleRef;
typedef const void *CFDictionaryRef;
typedef const void *CFAllocatorRef;
typedef struct { CFIndex version; void *retain; void *release; void *copyDescription; } CFDictionaryKeyCallBacks;
typedef CFDictionaryKeyCallBacks CFDictionaryValueCallBacks;
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFStringRef kCFBooleanTrue;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
#define kCFStringEncodingUTF8 0x08000100
extern CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef alloc, const UInt8 *path, CFIndex pathLen, bool isDirectory);
extern CFBundleRef CFBundleCreate(CFAllocatorRef alloc, CFURLRef url);
extern CFStringRef CFBundleGetIdentifier(CFBundleRef bundle);
extern bool CFStringGetCString(CFStringRef str, char *buffer, CFIndex bufferSize, unsigned encoding);
extern CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cStr, unsigned encoding);
extern CFDictionaryRef CFDictionaryCreate(CFAllocatorRef alloc, const void **keys, const void **values, CFIndex numValues, const CFDictionaryKeyCallBacks *keyCallBacks, const CFDictionaryValueCallBacks *valueCallBacks);
extern void CFRelease(const void *cf);
#endif
