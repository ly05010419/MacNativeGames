#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef DWORD (WINAPI *UndFn)(PCSTR, PSTR, DWORD, DWORD);
int main(int argc, char** argv) {
  HMODULE h = LoadLibraryA(argv[1]);
  if (!h) { printf("load failed %lu\n", GetLastError()); return 1; }
  UndFn f = (UndFn)GetProcAddress(h, "UnDecorateSymbolName");
  FILE* in = fopen(argv[2], "r"); FILE* out = fopen(argv[3], "w");
  char line[4096], buf[8192];
  while (fgets(line, sizeof line, in)) {
    line[strcspn(line, "\r\n")] = 0;
    DWORD n = f(line, buf, sizeof buf, 0x800);
    fprintf(out, "%s\n", n ? buf : "<FAIL>");
  }
  fclose(out);
  return 0;
}
