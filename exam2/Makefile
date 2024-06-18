CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -pthread -fsanitize=thread

# 源文件列表
SRCS = main.c channel_operation.c
OBJS = $(SRCS:.c=.o)

# 目標可執行文件
TARGET = myprogram

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean

clean:
	rm -f $(OBJS) $(TARGET)
