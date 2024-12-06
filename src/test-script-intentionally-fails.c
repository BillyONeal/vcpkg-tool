#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fputs("Bad arg count\n", stderr);
        return 1729;
    }

    int desired_exit_code;
    if (sscanf(argv[1], "%d", &desired_exit_code) == EOF)
    {
        fputs("That\'s not a number!", stderr);
        return 1729;
    }

    fputs("This is some error output that should be observable on the console\n", stderr);
    return desired_exit_code;
}
