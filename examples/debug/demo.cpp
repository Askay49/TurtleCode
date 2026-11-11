#include <iostream>
int add(int left, int right) {
    int result = left + right;
    return result;
}
int main() {
    int values[] = {3, 5, 8};
    int total = 0;
    for (int value : values) {
        total = add(total, value);
        std::cout << "value=" << value << ", total=" << total << '\n';
    }
    std::cout << "Done: " << total << '\n';
}
