#include <assert.h>

#include <lib/mhandle/mhandle.h>
#include <lib/util/debug.h>
#include <lib/util/string.h>

#include "stream-prefetcher.h"

struct str_map_t stream_request_kind_map =
{
	2, {
		{ "single", stream_request_single },
		{ "grouped", stream_request_grouped },
	}
};

