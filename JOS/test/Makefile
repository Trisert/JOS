# Convenience wrapper around Ceedling for the host unit-test suite.
# Ceedling (with its own Unity/CMock) comes from the gem: `gem install ceedling`.
.PHONY: test coverage clean native

test:
	ceedling test:all
	$(MAKE) native

# Compile the real C++ driver with deterministic HAL/RadioLib doubles.
native:
	mkdir -p build/native
	$(CXX) -std=c++11 -Wall -Wextra -Werror -Ifakes native/test_radio_lifecycle.cpp -o build/native/test_radio_lifecycle
	build/native/test_radio_lifecycle

coverage:
	ceedling gcov:all

clean:
	ceedling clobber
