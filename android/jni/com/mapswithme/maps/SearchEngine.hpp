#pragma once

#include "com/mapswithme/core/jni_helper.hpp"

#include "search/result.hpp"

#include <vector>

jobjectArray BuildSearchResults(search::Results const & results,
                                std::vector<bool> const & isLocalAdsCustomer,
                                bool hasPosition, double lat, double lon);