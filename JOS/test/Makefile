# Convenience wrapper around Ceedling for the host unit-test suite.
# Ceedling (with its own Unity/CMock) comes from the gem: `gem install ceedling`.
.PHONY: test coverage clean

test:
	ceedling test:all

coverage:
	ceedling gcov:all

clean:
	ceedling clobber
