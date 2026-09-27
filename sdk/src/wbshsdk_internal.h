#pragma once

/**
 * @file wbshsdk_internal.h
 * @brief What the two halves of the SDK share: the console half fills
 *        in its part of the table the loader hands to every util.
 */

#include "wbshsdk.h"

namespace wbshsdk_detail {

	void fillTerminalApi(WbshApi& api);

}  /* namespace wbshsdk_detail */
