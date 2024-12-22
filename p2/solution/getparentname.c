#include "types.h"
#include "stat.h"
#include "user.h"

int main(void) {
    char parentbuf[256];
    char childbuf[256];

    if (getparentname(parentbuf, childbuf, sizeof(parentbuf), sizeof(childbuf)) <= 0) {
        printf(1, "XV6_TEST_OUTPUT Failed to retrieve parent and child names\n");
        exit();
    }

    printf(1, "XV6_TEST_OUTPUT Parent name: %s\n", parentbuf);
    printf(1, "XV6_TEST_OUTPUT Child name: %s\n", childbuf);

    exit();
}
