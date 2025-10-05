#pragma once

#include "request.h"

namespace deploy {
    void verify_and_deploy(server::request& req);
}
