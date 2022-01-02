#pragma once

#include <stddef.h>
#include "bug.h"

#define DO_CONCAT(x, y) x##y
#define CONCAT(x, y) DO_CONCAT(x, y)
#define UNIQUE(x) CONCAT(x, __COUNTER__)

#define ARE_SAME_TYPE(x, y) __builtin_types_compatible_p(typeof(x), typeof(y))

#define DO_CONTAINER_OF(ptr, ptr_name, type, member) ({				       \
	char *ptr_name = (char*)(ptr);					                       \
	BUILD_BUG_ON(!ARE_SAME_TYPE(*(ptr), ((type*)sizeof(type))->member) &&  \
                 !ARE_SAME_TYPE(*(ptr), void));			                   \
	((type*)(ptr_name - offsetof(type, member))); })

#define container_of(ptr, type, member) DO_CONTAINER_OF(ptr, UNIQUE(uptr), type, member)

#define CEILING_DIVIDE(x, y) (!!(x) + (((x) - !!(x)) / (y)))