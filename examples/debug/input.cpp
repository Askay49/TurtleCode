#include <iostream>
int main() {
    int value = 0;
    std::cout << "Number: " << std::flush;
    std::cin >> value;
    const int result = value * 2;
    std::cout << "Input result: " << result << std::endl;
}
