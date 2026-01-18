#!/bin/bash
clang++ -std=c++11 -framework IOKit -framework CoreFoundation -I. TestClient.cpp -o TestClient
echo "✅ Compiled TestClient"