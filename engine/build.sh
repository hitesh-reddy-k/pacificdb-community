#!/bin/bash
# PacificDB Engine Build Script
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}🔨 Building PacificDB Engine...${NC}"

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Configure with CMake
echo -e "${YELLOW}⚙️  Running CMake configuration...${NC}"
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
echo -e "${YELLOW}🏗️  Compiling...${NC}"
make -j$(nproc) db_engine

if [ -f db_engine ]; then
    echo -e "${GREEN}✅ Build successful!${NC}"
    echo -e "Binary location: ${SCRIPT_DIR}/build/db_engine"
    echo ""
    echo "Run after configuring absolute storage paths:"
    echo "  cp .env.example .env && edit .env && ./start-node.sh"
    echo ""
    echo "Run with custom data path:"
    echo "  DATA_ROOT=/var/lib/pacificdb ./build/db_engine"
    echo ""
    echo "View all config options:"
    echo "  cat .env.example"
else
    echo -e "${RED}❌ Build failed${NC}"
    exit 1
fi
