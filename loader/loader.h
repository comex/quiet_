#include "decls.h"
#include "types.h"
#include "misc.h"
#if DUMMY
static inline bool in_right_process(void) { return true; }
#else // DUMMY
BEGIN_LOCAL_DECLS
extern bool g_self_fixup_done;
extern typeof(OSFatal) *OSFatal_ptr;
bool in_right_process(void);
END_LOCAL_DECLS
#endif // DUMMY
