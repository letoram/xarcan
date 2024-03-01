/***********************************************************

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

******************************************************************/

#ifndef DIX_REGISTRY_H
#define DIX_REGISTRY_H

/*
 * Result returned from any unsuccessful lookup
 */
#define XREGISTRY_UNKNOWN "<unknown>"

#include "resource.h"
#include "extnsionst.h"

#ifdef X_REGISTRY_REQUEST
extern _X_EXPORT void RegisterExtensionNames(ExtensionEntry * ext);

/*
 * Lookup functions.  The returned string must not be modified or freed.
 */
extern _X_EXPORT const char *LookupMajorName(int major);
extern _X_EXPORT const char *LookupRequestName(int major, int minor);
extern _X_EXPORT const char *LookupEventName(int event);
extern _X_EXPORT const char *LookupErrorName(int error);
#endif

#endif                          /* DIX_REGISTRY_H */
