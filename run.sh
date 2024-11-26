#!/bin/bash

# Cambiar a la carpeta especificada
cd Demo/CORTEX_LM3S811_GCC/ || { echo "No se pudo cambiar al directorio"; exit 1; }

# Ejecutar make clean
make clean

# Ejecutar make
make

# Ejecutar qemu con sudo
sudo qemu-system-arm -machine lm3s811evb -kernel ./gcc/RTOSDemo.axf -serial stdio
