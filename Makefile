TARGET = kilo

build: 
	cc $(TARGET).c -o $(TARGET) -Wall -Wextra -pedantic -std=c99

run:
	./kilo

clean: 
	$(RM) $(TARGET)