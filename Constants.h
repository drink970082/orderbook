#pragma once

#include <limits>
#include "Usings.h"

struct Constants
{
    static const Price InitialPrice = std::numeric_limits<Price>::quiet_NaN(); // since market orders don't have a price, we use NaN
};