# lanchat

# Makefile for MinGW-w64

CPP := x86_64-w64-mingw32-g++

all: lanchat.exe lanchat_static.exe

lanchat.exe: lanchat.cpp
	$(CPP) lanchat.cpp -l Xaudio2_9 -l Ole32 -l ksuser -l Ws2_32 -o lanchat.exe

lanchat_static.exe: lanchat.cpp
	$(CPP) lanchat.cpp -l Xaudio2_9 -l Ole32 -l ksuser -l Ws2_32 -static -s -Os -o lanchat_static.exe
