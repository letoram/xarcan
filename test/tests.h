#ifndef TESTS_H
#define TESTS_H

typedef void (*testfunc_t)(void);

const testfunc_t* fixes_test(void);
const testfunc_t* hashtabletest_test(void);
const testfunc_t* input_test(void);
const testfunc_t* list_test(void);
const testfunc_t* misc_test(void);
const testfunc_t* signal_logging_test(void);
const testfunc_t* string_test(void);
const testfunc_t* touch_test(void);
const testfunc_t* xfree86_test(void);
const testfunc_t* xkb_test(void);
const testfunc_t* xtest_test(void);
const testfunc_t* protocol_xchangedevicecontrol_test(void);
const testfunc_t* protocol_xiqueryversion_test(void);
const testfunc_t* protocol_xiquerydevice_test(void);
const testfunc_t* protocol_xiselectevents_test(void);
const testfunc_t* protocol_xigetselectedevents_test(void);
const testfunc_t* protocol_xisetclientpointer_test(void);
const testfunc_t* protocol_xigetclientpointer_test(void);
const testfunc_t* protocol_xipassivegrabdevice_test(void);
const testfunc_t* protocol_xiquerypointer_test(void);
const testfunc_t* protocol_xiwarppointer_test(void);
const testfunc_t* protocol_eventconvert_test(void);
const testfunc_t* xi2_test(void);

#ifndef INSIDE_PROTOCOL_COMMON

extern int enable_XISetEventMask_wrap;
extern int enable_GrabButton_wrap;

#endif /* INSIDE_PROTOCOL_COMMON */

#endif /* TESTS_H */

