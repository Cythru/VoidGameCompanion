#include "action_queue.h"
#include <cstring>

// This file contains the explicit template instantiation and any non-template
// helpers for the SPSC queue. The core queue logic lives in the header as
// it's a template, but we instantiate the default size here to ensure the
// symbols are available for linking.

namespace mc {

// Explicit instantiation of the default queue size
template class SPSCQueue<1024>;

} // namespace mc
