#ifndef LUMEN_ERROR_VIEWER_H
#define LUMEN_ERROR_VIEWER_H

#include "compositor.h"

void error_viewer_report(compositor_t *comp, const char *component,
                         const char *detail);
void error_viewer_poll(compositor_t *comp);

#endif
