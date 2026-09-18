# Convenience wrapper around Ceedling for the host unit-test suite.
# Ceedling (with its own Unity/CMock) comes from the gem: `gem install ceedling`.
.PHONY: test coverage clean native

NATIVE_CPPFLAGS := -std=c++11 -Wall -Wextra -Werror \
	-Wno-overloaded-virtual -Wno-unused-parameter -Wno-missing-field-initializers
NATIVE_INC := -Inative/include -I../Core/Inc/RadioLib/src -I../App/comms
NATIVE_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
NATIVE_CLOUD_INC := -Inative/cloud_include -Ifakes -I../App -I../App/payloads -I../App/obsw -I../App/memory -I../App/comms -I../Core/Inc

test:
	ceedling test:all
	$(MAKE) native

# Compile the real C++ driver with deterministic HAL/RadioLib doubles.
native:
	mkdir -p build/native
	$(CXX) $(NATIVE_CPPFLAGS) -Ifakes native/test_radio_lifecycle.cpp -o build/native/test_radio_lifecycle
	build/native/test_radio_lifecycle
	build/native/test_radio_lifecycle init-fail
	build/native/test_radio_lifecycle radio-fail
	$(CXX) $(NATIVE_CPPFLAGS) $(NATIVE_INC) \
		native/test_radiolib_hal.cpp ../App/comms/radiolib_hal.cpp \
		../Core/Inc/RadioLib/src/Hal.cpp -o build/native/test_radiolib_hal
	build/native/test_radiolib_hal
	$(CC) $(NATIVE_CFLAGS) $(NATIVE_CLOUD_INC) \
		native/test_cloud.c ../App/payloads/cloud.c -o build/native/test_cloud
	build/native/test_cloud

coverage:
	ceedling gcov:all

clean:
	ceedling clobber
