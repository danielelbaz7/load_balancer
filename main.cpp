#include "load_balancer.h"

int main() {
    LoadBalancer lb(8080);
    lb.run();
}
