#include <rw.h>
#include "host_checks.h"

int
RunHostChecks(rw::Camera *camera)
{
	return 0;
}

int
RunRestartHostChecks(bool (*restart)(void), rw::Camera *(*camera)(void))
{
	return 0;
}
