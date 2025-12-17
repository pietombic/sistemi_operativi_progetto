# MultiPandOS Project

### 1. Compilation
To compile the project and generate the necessary build files, run the following CMake commands from the project root directory:
```bash
cmake -B build
cmake --build build
```
### 2. Dependencies and Configuration

This project requires the uRISCV emulator. Please ensure it is installed on your system. ([uRISCV repository](https://github.com/virtualsquare/uriscv/))

After compilation, the config_machine.json file must be generated within the build folder. Navigate to the build directory and run make:

```bash
cd build
make
```
### 3. Execution
Once the config_machine.json is ready, follow these steps to launch the PandOS shell:
1. **Launch the Emulator**: Execute the uRISCV emulator from your terminal:
```bash
uriscv
```
2. **Load Configuration**: Open the generated config_machine.json file within the emulator interface.
3. **Start Execution**: Press the "Play" button (or the appropriate start command within the emulator GUI) to turn on the machine and begin execution.
4. **Open Terminal**: Navigate to the "Window" menu and select "Terminal 0". The PandOS shell prompt should now be visible.
## Phase 1: Queue Management Implementation

We successfully completed the first phase of the project by implementing the second layer, the Queue Manager.

This involved developing core functionalities for:
- **PCB (Process Control Block) Management**: Handling the allocation and deallocation of PCBs.
- **PCB Queue Management**: Managing the queues of processes.
- **PCT (Process Control Table) Tree Management**: Implementing the control structure for processes.

The underlying list structures were designed and built following the robust Double Linked List model used within the Linux Kernel.

This phase was a highly rewarding and significant learning experience, as it marked the first time the entire team worked with and implemented this specific Kernel-style list model.


---

Stay hungry, stay foolish
