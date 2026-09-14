// T01 hello-world: proves the toolchain produces a binary this device runs.
#include <cstdio>
#include <sys/utsname.h>

int main()
{
    utsname u{};
    if (uname(&u) != 0) {
        std::perror("uname");
        return 1;
    }
    std::printf("sumiyomi hello: %s %s %s\n", u.sysname, u.release, u.machine);
    return 0;
}
