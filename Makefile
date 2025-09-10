TARGETS = np_simple np_single_proc

all: $(TARGETS)

np_simple: np_simple.cpp
	g++ np_simple.cpp -o np_simple

np_single_proc: np_single_proc.cpp
	g++ np_single_proc.cpp -o np_single_proc

clean:
	rm -f $(TARGETS)