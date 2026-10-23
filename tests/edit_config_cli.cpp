#include "edit_config.h"
#include <string>
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) return 2;
    return edit_config::command(argv[1], std::string(argv[2]) == "save", argv[3], argc == 5 ? argv[4] : "");
}
