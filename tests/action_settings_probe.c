#include <assert.h>
#include "../games/totk/runtime/runtime.h"
void runtime_input_clear(void){}
#include "../games/totk/runtime/action_runtime.h"
int main(int argc,char **argv){
 assert(argc==4);setenv("TOTK_CONTROLS",argv[1],1);RuntimeActionBindings b;runtime_action_bindings_get(&b);
 assert(runtime_action_bindings_enabled()==atoi(argv[2]));assert(b.target[0]==atoi(argv[3]));
 puts("PASS: optional actions startup and persisted settings");return 0;
}
