ESP32­S3 

Technical Reference Manual 

Version 1.2 

Espressif Systems 

Copyright © 2023 

www.espressif.com  
About This Document 

The ESP32­S3 is targeted at developers working on low level software projects that use the ESP32-S3 SoC. It describes the hardware modules listed below for the ESP32-S3 SoC and other products in ESP32-S3 series. The modules detailed in this document provide an overview, list of features, hardware architecture details, any necessary programming procedures, as well as register descriptions. 

Navigation in This Document 

Here are some tips on navigation through this extensive document: 

• Release Status at a Glance on the very next page is a minimal list of all chapters from where you can directly jump to a specific chapter. 

• Use the Bookmarks on the side bar to jump to any specific chapters or sections from anywhere in the document. Note this PDF document is configured to automatically display Bookmarks when open, which is necessary for an extensive document like this one. However, some PDF viewers or browsers ignore this setting, so if you don’t see the Bookmarks by default, try one or more of the following methods: 

– Install a PDF Reader Extension for your browser; 

– Download this document, and view it with your local PDF viewer; 

– Set your PDF viewer to always automatically display the Bookmarks on the left side bar when open. 

• Use the native Navigation function of your PDF viewer to navigate through the documents. Most PDF viewers support to go Up, Down, Previous, Next, Back, Forward and Page with buttons, menu or hot keys. 

• You can also use the built-in GoBack button on the upper right corner on each and every page to go back to the previous place before you click a link within the document. Note this feature may only work with some Acrobat-specific PDF viewers (for example, Acrobat Reader and Adobe DC) and browsers with built-in Acrobat-specific PDF viewers or extensions (for example, Firefox).  
Release Status at a Glance 

| No.  | ESP32­S3 Chapters  | Progress  | No.  | ESP32­S3 Chapters  | Progress |
| :---: | ----- | :---: | ----- | :---- | ----- |
| 1  | Processor Instruction Extensions (PIE)  | Published  | 21  | SHA Accelerator (SHA)  | Published |
| 2 | ULP Coprocessor (ULP-FSM,  ULP-RISC-V) | Published  | 22  | Digital Signature (DS)  | Published |
| 3  | GDMA Controller (GDMA)  | Published  | 23 | External Memory Encryption and Decryption (XTS\_AES) | Published |
| 4  | System and Memory  | Published  | 24  | Random Number Generator (RNG)  | Published |
| 5  | eFuse Controller  | Published  | 25  | Clock Glitch Detection  | Published |
| 6 | IO MUX and GPIO Matrix (GPIO, IO MUX) | Published  | 26  | UART Controller (UART)  | Published |
| 7  | Reset and Clock  | Published  | 27  | SPI Controller (SPI)  | Published |
| 8  | Chip Boot Control  | Published  | 28  | I2C Controller (I2C)  | Published |
| 9  | Interrupt Matrix (INTERRUPT)  | Published  | 29  | I2S Controller (I2S)  | Published |
| 10  | Low-power Management (RTC\_CNTL)  | Published  | 30  | Pulse Count Controller (PCNT)  | Published |
| 11  | System Timer (SYSTIMER)  | Published  | 31  | USB On-The-Go (USB)  | Published |
| 12  | Timer Group (TIMG)  | Published  | 32 | USB Serial/JTAG Controller  (USB\_SERIAL\_JTAG) | Published |
| 13  | Watchdog Timers (WDT)  | Published  | 33 | Two-wire Automotive Interface  (TWAI®) | Published |
| 14  | XTAL32K Watchdog Timers (XTWDT)  | Published  | 34  | SD/MMC Host Controller (SDHOST)  | Published |
| 15  | Permission Control (PMS)  | Published  | 35  | LED PWM Controller (LEDC)  | Published |
| 16  | World Controller (WCL)  | Published  | 36  | Motor Control PWM (MCPWM)  | Published |
| 17  | System Registers (SYSTEM)  | Published  | 37  | Remote Control Peripheral (RMT)  | Published |
| 18  | AES Accelerator (AES)  | Published  | 38 | LCD and Camera Controller  (LCD\_CAM) | Published |
| 19  | HMAC Accelerator (HMAC)  | Published  | 39 | On-Chip Sensors and Analog Signal Processing | Published |
| 20  | RSA Accelerator (RSA)  | Published |  |  |  |

Note: 

Check the link or the QR code to make sure that you use the latest version of this document: 

https://www.espressif.com/documentation/esp32-s3\_technical\_reference\_manual\_en.pdf  
Contents GoBack Contents 

1 Processor Instruction Extensions (PIE) 36 1.1 Overview 36 1.2 Features 36 1.3 Structure Overview 36 

1.3.1 Bank of Vector Registers 37 1.3.2 ALU 38 1.3.3 QACC Accumulator Register 38 1.3.4 ACCX Accumulator Register 38 1.3.5 Address Unit 38 

1.4 Syntax Description 38 1.4.1 Bit/Byte Order 39 1.4.2 Instruction Field Definition 40 

1.5 Components of Extended Instruction Set 42 1.5.1 Registers 42 1.5.1.1 General-Purpose Registers 43 1.5.1.2 Special Registers 43 1.5.2 Fast GPIO Interface 45 1.5.2.1 GPIO\_OUT 45 1.5.2.2 GPIO\_IN 45 1.5.3 Data Format and Alignment 45 1.5.4 Data Overflow and Saturation Handling 46 1.6 Extended Instruction List 47 1.6.1 Read Instructions 49 1.6.2 Write Instructions 50 1.6.3 Data Exchange Instructions 51 1.6.4 Arithmetic Instructions 52 1.6.5 Comparison Instructions 56 1.6.6 Bitwise Logical Instructions 57 1.6.7 Shift Instructions 57 1.6.8 FFT Dedicated Instructions 58 1.6.9 GPIO Control Instructions 59 1.6.10 Processor Control Instructions 59 1.7 Instruction Performance 61 1.7.1 Data Hazard 61 1.7.2 Hardware Resource Hazard 70 1.7.3 Control Hazard 70 1.8 Extended Instruction Functional Description 72 

2 ULP Coprocessor (ULP­FSM, ULP­RISC­V) 296 2.1 Overview 296 2.2 Features 296 2.3 Programming Workflow 297 

Espressif Systems 4 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

2.4 ULP Coprocessor Sleep and Weakup Workflow 298 2.5 ULP-FSM 300 2.5.1 Features 300 2.5.2 Instruction Set 300 2.5.2.1 ALU \- Perform Arithmetic and Logic Operations 301 2.5.2.2 ST – Store Data in Memory 303 2.5.2.3 LD – Load Data from Memory 306 2.5.2.4 JUMP – Jump to an Absolute Address 307 2.5.2.5 JUMPR – Jump to a Relative Address (Conditional upon R0) 307 2.5.2.6 JUMPS – Jump to a Relative Address (Conditional upon Stage Count Register) 308 2.5.2.7 HALT – End the Program 309 2.5.2.8 WAKE – Wake up the Chip 309 2.5.2.9 WAIT – Wait for a Number of Cycles 309 2.5.2.10 TSENS – Take Measurement with Temperature Sensor 310 2.5.2.11 ADC – Take Measurement with ADC 310 2.5.2.12 REG\_RD – Read from Peripheral Register 311 2.5.2.13 REG\_WR – Write to Peripheral Register 311 2.6 ULP-RISC-V 312 2.6.1 Features 312 2.6.2 Multiplier and Divider 312 2.6.3 ULP-RISC-V Interrupts 313 2.6.3.1 Introduction 313 2.6.3.2 Interrupt Controller 313 2.6.3.3 Interrupt Instructions 314 2.6.3.4 RTC Peripheral Interrupts 315 2.7 RTC I2C Controller 316 2.7.1 Connecting RTC I2C Signals 316 2.7.2 Configuring RTC I2C 317 2.7.3 Using RTC I2C 317 2.7.3.1 Instruction Format 317 2.7.3.2 I2C\_RD \- I2C Read Workflow 318 2.7.3.3 I2C\_WR \- I2C Write Workflow 318 2.7.3.4 Detecting Error Conditions 319 2.7.4 RTC I2C Interrupts 319 2.8 Address Mapping 320 2.9 Register Summary 320 2.9.1 ULP (ALWAYS\_ON) Register Summary 320 2.9.2 ULP (RTC\_PERI) Register Summary 321 2.9.3 RTC I2C (RTC\_PERI) Register Summary 321 2.9.4 RTC I2C (I2C) Register Summary 321 2.10 Registers 322 2.10.1 ULP (ALWAYS\_ON) Registers 322 2.10.2 ULP (RTC\_PERI) Registers 325 2.10.3 RTC I2C (RTC\_PERI) Registers 329 2.10.4 RTC I2C (I2C) Registers 331 

Espressif Systems 5 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

3 GDMA Controller (GDMA) 345 3.1 Overview 345 3.2 Features 345 3.3 Architecture 346 3.4 Functional Description 347 

3.4.1 Linked List 347 3.4.2 Peripheral-to-Memory and Memory-to-Peripheral Data Transfer 348 3.4.3 Memory-to-Memory Data Transfer 348 3.4.4 Channel Buffer 349 3.4.5 Enabling GDMA 349 3.4.6 Linked List Reading Process 350 3.4.7 EOF 350 3.4.8 Accessing Internal RAM 351 3.4.9 Accessing External RAM 351 3.4.10 External RAM Access Permissions 352 3.4.11 Seamless Access to Internal and External RAM 353 3.4.12 Arbitration 353 

3.5 GDMA Interrupts 353 3.6 Programming Procedures 354 3.6.1 Programming Procedures for GDMA’s Transmit Channel 354 3.6.2 Programming Procedures for GDMA’s Receive Channel 354 3.6.3 Programming Procedures for Memory-to-Memory Transfer 354 3.7 Register Summary 356 3.8 Registers 362 

4 System and Memory 385 4.1 Overview 385 4.2 Features 385 4.3 Functional Description 386 

4.3.1 Address Mapping 386 4.3.2 Internal Memory 387 4.3.3 External Memory 390 

4.3.3.1 External Memory Address Mapping 390 4.3.3.2 Cache 390 4.3.3.3 Cache Operations 391 

4.3.4 GDMA Address Space 392 4.3.5 Modules/Peripherals 393 4.3.5.1 Module/Peripheral Address Mapping 393 

5 eFuse Controller 396 5.1 Overview 396 5.2 Features 396 5.3 Functional Description 396 

5.3.1 Structure 396 5.3.1.1 EFUSE\_WR\_DIS 403 5.3.1.2 EFUSE\_RD\_DIS 403 

Espressif Systems 6 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

5.3.1.3 Data Storage 403 5.3.2 Programming of Parameters 404 5.3.3 User Read of Parameters 406 5.3.4 eFuse VDDQ Timing 408 5.3.5 The Use of Parameters by Hardware Modules 408 5.3.6 Interrupts 408 

5.4 Register Summary 409 5.5 Registers 413 

6 IO MUX and GPIO Matrix (GPIO, IO MUX) 457 6.1 Overview 457 6.2 Features 457 6.3 Architectural Overview 457 6.4 Peripheral Input via GPIO Matrix 459 

6.4.1 Overview 459 6.4.2 Signal Synchronization 459 6.4.3 Functional Description 460 6.4.4 Simple GPIO Input 461 

6.5 Peripheral Output via GPIO Matrix 461 6.5.1 Overview 461 6.5.2 Functional Description 462 6.5.3 Simple GPIO Output 463 6.5.4 Sigma Delta Modulated Output 463 

6.5.4.1 Functional Description 463 6.5.4.2 SDM Configuration 464 6.6 Direct Input and Output via IO MUX 464 6.6.1 Overview 464 6.6.2 Functional Description 464 6.7 RTC IO MUX for Low Power and Analog Input/Output 464 6.7.1 Overview 464 6.7.2 Low Power Capabilities 465 6.7.3 Analog Functions 465 6.8 Pin Functions in Light-sleep 465 6.9 Pin Hold Feature 466 6.10 Power Supply and Management of GPIO Pins 466 6.10.1 Power Supply of GPIO Pins 466 6.10.2 Power Supply Management 466 6.11 Peripheral Signals via GPIO Matrix 466 6.12 IO MUX Function List 478 6.13 RTC IO MUX Pin List 479 6.14 Register Summary 481 6.14.1 GPIO Matrix Register Summary 481 6.14.2 IO MUX Register Summary 482 6.14.3 SDM Output Register Summary 484 6.14.4 RTC IO MUX Register Summary 484 6.15 Registers 486 

Espressif Systems 7 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

6.15.1 GPIO Matrix Registers 486 6.15.2 IO MUX Registers 497 6.15.3 SDM Output Registers 499 6.15.4 RTC IO MUX Registers 501 

7 Reset and Clock 510 7.1 Reset 510 7.1.1 Overview 510 7.1.2 Architectural Overview 510 7.1.3 Features 510 7.1.4 Functional Description 511 7.2 Clock 512 7.2.1 Overview 512 7.2.2 Architectural Overview 512 7.2.3 Features 512 7.2.4 Functional Description 513 7.2.4.1 CPU Clock 513 7.2.4.2 Peripheral Clocks 513 7.2.4.3 Wi-Fi and Bluetooth LE Clock 515 7.2.4.4 RTC Clock 515 

8 Chip Boot Control 516 8.1 Overview 516 8.2 Boot Mode Control 517 8.3 ROM Messages Printing Control 517 8.4 VDD\_SPI Voltage Control 518 8.5 JTAG Signal Source Control 519 

9 Interrupt Matrix (INTERRUPT) 520 9.1 Overview 520 9.2 Features 520 9.3 Functional Description 521 

9.3.1 Peripheral Interrupt Sources 521 9.3.2 CPU Interrupts 525 9.3.3 Allocate Peripheral Interrupt Source to CPU*x* Interrupt 526 

9.3.3.1 Allocate one peripheral interrupt source (Source\_*Y*) to CPU*x* 526 9.3.3.2 Allocate multiple peripheral interrupt sources (Source\_Y*n*) to CPU*x* 527 9.3.3.3 Disable CPU*x* peripheral interrupt source (Source\_*Y*) 527 

9.3.4 Disable CPU*x* NMI Interrupt 527 9.3.5 Query Current Interrupt Status of Peripheral Interrupt Source 527 9.4 Register Summary 527 9.4.1 CPU0 Interrupt Register Summary 528 9.4.2 CPU1 Interrupt Register Summary 531 9.5 Registers 536 9.5.1 CPU0 Interrupt Registers 536 9.5.2 CPU1 Interrupt Registers 540 

Espressif Systems 8 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

10 Low­power Management (RTC\_CNTL) 546 10.1 Introduction 546 10.2 Features 546 10.3 Functional Description 546 

10.3.1 Power Management Unit 548 10.3.2 Low-Power Clocks 549 10.3.3 Timers 551 10.3.4 Voltage Regulators 552 

10.3.4.1 Digital Voltage Regulator 552 10.3.4.2 Low-power Voltage Regulator 553 10.3.4.3 Flash Voltage Regulator 553 10.3.4.4 Brownout Detector 554 

10.4 Power Modes Management 555 10.4.1 Power Domain 555 10.4.2 RTC States 557 10.4.3 Pre-defined Power Modes 558 10.4.4 Wakeup Source 559 10.4.5 Reject Sleep 560 

10.5 Retention DMA 561 10.6 RTC Boot 562 10.7 Register Summary 564 10.8 Registers 566 

11 System Timer (SYSTIMER) 613 11.1 Overview 613 11.2 Features 613 11.3 Clock Source Selection 614 11.4 Functional Description 614 

11.4.1 Counter 614 11.4.2 Comparator and Alarm 615 11.4.3 Synchronization Operation 616 11.4.4 Interrupt 617 

11.5 Programming Procedure 617 11.5.1 Read Current Count Value 617 11.5.2 Configure One-Time Alarm in Target Mode 617 11.5.3 Configure Periodic Alarms in Period Mode 617 11.5.4 Update After Deep-sleep and Light-sleep 618 

11.6 Register Summary 618 11.7 Registers 620 

12 Timer Group (TIMG) 633 12.1 Overview 633 12.2 Functional Description 634 

12.2.1 16-bit Prescaler and Clock Selection 634 12.2.2 54-bit Time-base Counter 634 12.2.3 Alarm Generation 634 

Espressif Systems 9 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

12.2.4 Timer Reload 635 12.2.5 RTC\_SLOW\_CLK Frequency Calculation 636 12.2.6 Interrupts 636 

12.3 Configuration and Usage 637 12.3.1 Timer as a Simple Clock 637 12.3.2 Timer as One-shot Alarm 637 12.3.3 Timer as Periodic Alarm 637 12.3.4 RTC\_SLOW\_CLK Frequency Calculation 638 

12.4 Register Summary 639 12.5 Registers 641 

13 Watchdog Timers (WDT) 651 13.1 Overview 651 13.2 Digital Watchdog Timers 652 

13.2.1 Features 652 13.2.2 Functional Description 653 13.2.2.1 Clock Source and 32-Bit Counter 653 13.2.2.2 Stages and Timeout Actions 654 13.2.2.3 Write Protection 654 13.2.2.4 Flash Boot Protection 655 13.3 Super Watchdog 655 13.3.1 Features 655 13.3.2 Super Watchdog Controller 655 13.3.2.1 Structure 656 13.3.2.2 Workflow 656 13.4 Interrupts 656 13.5 Registers 657 

14 XTAL32K Watchdog Timers (XTWDT) 658 14.1 Overview 658 14.2 Features 658 

14.2.1 Interrupt and Wake-Up 658 14.2.2 BACKUP32K\_CLK 658 14.3 Functional Description 658 14.3.1 Workflow 659 14.3.2 BACKUP32K\_CLK Working Principle 659 14.3.3 Configuring the Divisor Component of BACKUP32K\_CLK 659 

15 Permission Control (PMS) 661 15.1 Overview 661 15.2 Features 661 15.3 Internal Memory 661 

15.3.1 ROM 662 15.3.1.1 Address 662 15.3.1.2 Access Configuration 662 15.3.2 SRAM 663 

Espressif Systems 10 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

15.3.2.1 Address 663 15.3.2.2 Internal SRAM0 Access Configuration 663 15.3.2.3 Internal SRAM1 Access Configuration 664 15.3.2.4 Internal SRAM2 Access Configuration 668 

15.3.3 RTC FAST Memory 669 15.3.3.1 Address 669 15.3.3.2 Access Configuration 669 

15.3.4 RTC SLOW Memory 670 15.3.4.1 Address 670 15.3.4.2 Access Configuration 670 

15.4 Peripherals 671 15.4.1 Access Configuration 671 15.4.2 Split Peripheral Regions into Split Regions 673 

15.5 External Memory 673 15.5.1 Address 674 15.5.2 Access Configuration 674 15.5.3 GDMA 675 

15.6 Unauthorized Access and Interrupts 676 15.6.1 Interrupt upon Unauthorized IBUS Access 676 15.6.2 Interrupt upon Unauthorized DBUS Access 677 15.6.3 Interrupt upon Unauthorized Access to External Memory 677 15.6.4 Interrupt upon Unauthorized Access to Internal Memory via GDMA 678 15.6.5 Interrupt upon Unauthorized peripheral bus (PIF) Access 678 15.6.6 Interrupt upon Unauthorized PIF Access Alignment 679 

15.7 Protection of CPU VECBASE Registers 680 15.8 Register Locks 681 15.9 Register Summary 684 15.10 Registers 689 

16 World Controller (WCL) 779 16.1 Introduction 779 16.2 Features 779 16.3 Functional Description 779 16.4 CPU’s World Switch 781 

16.4.1 From Secure World to Non-secure World 781 16.4.2 From Non-secure World to Secure World 782 16.4.3 Clearing the write\_buffer 783 

16.5 World Switch Log 784 16.5.1 Structure of World Switch Log Register 784 16.5.2 How World Switch Log Registers are Updated 784 16.5.3 How to Read World Switch Log Registers 786 16.5.4 Nested Interrupts 787 

16.5.4.1 How to Handle Nested Interrupts 787 16.5.4.2 Programming Procedure 787 16.6 NMI Interrupt Masking 788 16.7 Register Summary 790 

Espressif Systems 11 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

16.8 Registers 791 

17 System Registers (SYSTEM) 799 17.1 Overview 799 17.2 Features 799 17.3 Function Description 799 

17.3.1 System and Memory Registers 799 17.3.1.1 Internal Memory 799 17.3.1.2 External Memory 800 17.3.1.3 RSA Memory 800 

17.3.2 Clock Registers 801 17.3.3 Interrupt Signal Registers 801 17.3.4 Low-power Management Registers 801 17.3.5 Peripheral Clock Gating and Reset Registers 801 17.3.6 CPU Control Registers 803 

17.4 Register Summary 804 17.5 Registers 805 

18 SHA Accelerator (SHA) 819 18.1 Introduction 819 18.2 Features 819 18.3 Working Modes 819 18.4 Function Description 820 

18.4.1 Preprocessing 820 18.4.1.1 Padding the Message 820 18.4.1.2 Parsing the Message 821 18.4.1.3 Initial Hash Value 821 

18.4.2 Hash task Process 822 18.4.2.1 Typical SHA Mode Process 822 18.4.2.2 DMA-SHA Mode Process 824 

18.4.3 Message Digest 826 18.4.4 Interrupt 827 18.5 Register Summary 827 18.6 Registers 828 

19 AES Accelerator (AES) 833 19.1 Introduction 833 19.2 Features 833 19.3 AES Working Modes 833 19.4 Typical AES Working Mode 834 

19.4.1 Key, Plaintext, and Ciphertext 834 19.4.2 Endianness 835 19.4.3 Operation Process 837 

19.5 DMA-AES Working Mode 837 19.5.1 Key, Plaintext, and Ciphertext 838 19.5.2 Endianness 838 

Espressif Systems 12 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

19.5.3 Standard Incrementing Function 839 19.5.4 Block Number 839 19.5.5 Initialization Vector 839 19.5.6 Block Operation Process 840 

19.6 Memory Summary 840 19.7 Register Summary 841 19.8 Registers 842 

20 RSA Accelerator (RSA) 846 20.1 Introduction 846 20.2 Features 846 20.3 Functional Description 846 

20.3.1 Large Number Modular Exponentiation 846 20.3.2 Large Number Modular Multiplication 848 20.3.3 Large Number Multiplication 848 20.3.4 Options for Acceleration 849 

20.4 Memory Summary 850 20.5 Register Summary 851 20.6 Registers 851 

21 HMAC Accelerator (HMAC) 855 21.1 Main Features 855 21.2 Functional Description 855 

21.2.1 Upstream Mode 855 21.2.2 Downstream JTAG Enable Mode 856 21.2.3 Downstream Digital Signature Mode 856 21.2.4 HMAC eFuse Configuration 856 21.2.5 HMAC Initialization 857 21.2.6 HMAC Process (Detailed) 857 

21.3 HMAC Algorithm Details 859 21.3.1 Padding Bits 859 21.3.2 HMAC Algorithm Structure 860 

21.4 Register Summary 862 21.5 Registers 864 

22 Digital Signature (DS) 870 22.1 Overview 870 22.2 Features 870 22.3 Functional Description 870 

22.3.1 Overview 870 22.3.2 Private Key Operands 871 22.3.3 Software Prerequisites 871 22.3.4 DS Operation at the Hardware Level 872 22.3.5 DS Operation at the Software Level 873 

22.4 Memory Summary 875 22.5 Register Summary 876 

Espressif Systems 13 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

22.6 Registers 877 

23 External Memory Encryption and Decryption (XTS\_AES)879 23.1 Overview 879 23.2 Features 879 23.3 Module Structure 879 23.4 Functional Description 880 

23.4.1 XTS Algorithm 880 23.4.2 Key 880 23.4.3 Target Memory Space 881 23.4.4 Data Padding 881 23.4.5 Manual Encryption Block 882 23.4.6 Auto Encryption Block 883 23.4.7 Auto Decryption Block 883 

23.5 Software Process 884 23.6 Register Summary 885 23.7 Registers 886 

24 Clock Glitch Detection 889 24.1 Overview 889 24.2 Functional Description 889 

24.2.1 Clock Glitch Detection 889 24.2.2 Reset 889 

25 Random Number Generator (RNG) 890 25.1 Introduction 890 25.2 Features 890 25.3 Functional Description 890 25.4 Programming Procedure 891 25.5 Register Summary 891 25.6 Register 891 

26 UART Controller (UART) 892 26.1 Overview 892 26.2 Features 892 26.3 UART Structure 893 26.4 Functional Description 894 

26.4.1 Clock and Reset 894 26.4.2 UART RAM 895 26.4.3 Baud Rate Generation and Detection 896 

26.4.3.1 Baud Rate Generation 896 26.4.3.2 Baud Rate Detection 897 26.4.4 UART Data Frame 898 26.4.5 AT\_CMD Character Structure 898 26.4.6 RS485 899 26.4.6.1 Driver Control 899 

Espressif Systems 14 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

26.4.6.2 Turnaround Delay 899 26.4.6.3 Bus Snooping 900 26.4.7 IrDA 900 26.4.8 Wake-up 901 26.4.9 Loopback Test 901 26.4.10 Flow Control 901 26.4.10.1 Hardware Flow Control 902 26.4.10.2 Software Flow Control 903 26.4.11 GDMA Mode 903 26.4.12 UART Interrupts 904 26.4.13 UHCI Interrupts 905 26.5 Programming Procedures 905 26.5.1 Register Type 906 26.5.1.1 Synchronous Registers 906 26.5.1.2 Static Registers 907 26.5.1.3 Immediate Registers 908 26.5.2 Detailed Steps 908 26.5.2.1 Initializing UART*n* 908 26.5.2.2 Configuring UART*n* Communication 909 26.5.2.3 Enabling UART*n* 909 26.6 Register Summary 911 26.6.1 UART Register Summary 911 26.6.2 UHCI Register Summary 912 26.7 Registers 914 26.7.1 UART Registers 914 26.7.2 UHCI Regsiters 934 

27 I2C Controller (I2C) 953 27.1 Overview 953 27.2 Features 953 27.3 I2C Architecture 954 27.4 Functional Description 956 

27.4.1 Clock Configuration 956 27.4.2 SCL and SDA Noise Filtering 956 27.4.3 SCL Clock Stretching 957 27.4.4 Generating SCL Pulses in Idle State 957 27.4.5 Synchronization 957 27.4.6 Open-Drain Output 958 27.4.7 Timing Parameter Configuration 959 27.4.8 Timeout Control 960 27.4.9 Command Configuration 961 27.4.10 TX/RX RAM Data Storage 962 27.4.11 Data Conversion 963 27.4.12 Addressing Mode 963 27.4.13 *R*/*W* Bit Check in 10-bit Addressing Mode 963 27.4.14 To Start the I2C Controller 964 

Espressif Systems 15 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

27.5 Programming Example 964 27.5.1 I2Cmaster Writes to I2Cslave with a 7-bit Address in One Command Sequence 964 27.5.1.1 Introduction 964 27.5.1.2 Configuration Example 965 27.5.2 I2Cmaster Writes to I2Cslave with a 10-bit Address in One Command Sequence 966 27.5.2.1 Introduction 966 27.5.2.2 Configuration Example 966 27.5.3 I2Cmaster Writes to I2Cslave with Two 7-bit Addresses in One Command Sequence 967 27.5.3.1 Introduction 968 27.5.3.2 Configuration Example 968 27.5.4 I2Cmaster Writes to I2Cslave with a 7-bit Address in Multiple Command Sequences 969 27.5.4.1 Introduction 970 27.5.4.2 Configuration Example 971 27.5.5 I2Cmaster Reads I2Cslave with a 7-bit Address in One Command Sequence 972 27.5.5.1 Introduction 972 27.5.5.2 Configuration Example 973 27.5.6 I2Cmaster Reads I2Cslave with a 10-bit Address in One Command Sequence 974 27.5.6.1 Introduction 974 27.5.6.2 Configuration Example 975 27.5.7 I2Cmaster Reads I2Cslave with Two 7-bit Addresses in One Command Sequence 976 27.5.7.1 Introduction 976 27.5.7.2 Configuration Example 977 27.5.8 I2Cmaster Reads I2Cslave with a 7-bit Address in Multiple Command Sequences 978 27.5.8.1 Introduction 979 27.5.8.2 Configuration Example 980 27.6 Interrupts 981 27.7 Register Summary 983 27.8 Registers 985 

28 I2S Controller (I2S) 1005 28.1 Overview 1005 28.2 Terminology 1005 28.3 Features 1006 28.4 System Architecture 1007 28.5 Supported Audio Standards 1008 

28.5.1 TDM Philips Standard 1009 28.5.2 TDM MSB Alignment Standard 1009 28.5.3 TDM PCM Standard 1010 28.5.4 PDM Standard 1010 

28.6 TX/RX Clock 1011 28.7 I2S*n* Reset 1013 28.8 I2S*n* Master/Slave Mode 1013 

28.8.1 Master/Slave TX Mode 1013 28.8.2 Master/Slave RX Mode 1014 28.9 Transmitting Data 1014 28.9.1 Data Format Control 1014 

Espressif Systems 16 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

28.9.1.1 Bit Width Control of Channel Valid Data 1014 28.9.1.2 Endian Control of Channel Valid Data 1015 28.9.1.3 A-law/*µ*\-law Compression and Decompression 1015 28.9.1.4 Bit Width Control of Channel TX Data 1015 28.9.1.5 Bit Order Control of Channel Data 1016 

28.9.2 Channel Mode Control 1016 28.9.2.1 I2S*n* Channel Control in TDM Mode 1017 28.9.2.2 I2S*n* Channel Control in PDM Mode 1017 

28.10 Receiving Data 1020 28.10.1 Channel Mode Control 1020 28.10.1.1 I2S*n* Channel Control in TDM Mode 1020 28.10.1.2 I2S*n* Channel Control in PDM Mode 1021 28.10.2 Data Format Control 1021 28.10.2.1 Bit Order Control of Channel Data 1022 28.10.2.2 Bit Width Control of Channel Storage (Valid) Data 1022 28.10.2.3 Bit Width Control of Channel RX Data 1022 28.10.2.4 Endian Control of Channel Storage Data 1022 28.10.2.5 A-law/*µ*\-law Compression and Decompression 1023 28.11 Software Configuration Process 1023 28.11.1 Configure I2S*n* as TX Mode 1023 28.11.2 Configure I2S*n* as RX Mode 1024 28.12 I2S*n* Interrupts 1024 28.13 Register Summary 1025 28.14 Registers 1026 

29 LCD and Camera Controller (LCD\_CAM) 1043 29.1 Overview 1043 29.2 Features 1043 29.3 Functional Description 1043 

29.3.1 Block Diagram 1043 29.3.2 Signal Description 1044 29.3.3 LCD\_CAM Module Clocks 1045 

29.3.3.1 LCD Clock 1045 29.3.3.2 Camera Clock 1046 29.3.4 LCD\_CAM Reset 1047 29.3.5 LCD\_CAM Data Format Control 1047 29.3.5.1 LCD Data Format Control 1047 29.3.5.2 Camera Data Format Control 1048 29.3.6 YUV-RGB Data Format Conversion 1049 29.3.6.1 YUV Timing 1050 29.3.6.2 Data Conversion Configuration 1050 29.4 Software Configuration Process 1051 29.4.1 Configure LCD (RGB Format) as TX Mode 1052 29.4.2 Configure LCD (I8080/MOTO6800 Format) as TX Mode 1053 29.4.3 Configure Camera as RX Mode 1055 29.5 LCD\_CAM Interrupts 1056 

Espressif Systems 17 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

29.6 Register Summary 1057 29.7 Registers 1058 

30 SPI Controller (SPI) 1071 30.1 Overview 1071 30.2 Glossary 1071 30.3 Features 1072 30.4 Architectural Overview 1074 30.5 Functional Description 1074 

30.5.1 Data Modes 1074 30.5.2 Introduction to FSPI bus and SPI3 Bus Signals 1075 30.5.3 Bit Read/Write Order Control 1078 30.5.4 Transfer Modes 1080 30.5.5 CPU-Controlled Data Transfer 1080 

30.5.5.1 CPU-Controlled Master Mode 1080 30.5.5.2 CPU-Controlled Slave Mode 1081 30.5.6 DMA-Controlled Data Transfer 1082 30.5.6.1 GDMA Configuration 1082 30.5.6.2 GDMA TX/RX Buffer Length Control 1083 30.5.7 Data Flow Control in GP-SPI Master and Slave Modes 1083 30.5.7.1 GP-SPI Functional Blocks 1084 30.5.7.2 Data Flow Control in Master Mode 1085 30.5.7.3 Data Flow Control in Slave Mode 1085 30.5.8 GP-SPI Works as a Master 1086 30.5.8.1 State Machine 1087 30.5.8.2 Register Configuration for State and Bit Mode Control 1089 30.5.8.3 Full-Duplex Communication (1-bit Mode Only) 1093 30.5.8.4 Half-Duplex Communication (1/2/4/8-bit Mode) 1094 30.5.8.5 DMA-Controlled Configurable Segmented Transfer 1096 30.5.9 GP-SPI Works as a Slave 1100 30.5.9.1 Communication Formats 1100 30.5.9.2 Supported CMD Values in Half-Duplex Communication 1101 30.5.9.3 Slave Single Transfer and Slave Segmented Transfer 1104 30.5.9.4 Configuration of Slave Single Transfer 1104 30.5.9.5 Configuration of Slave Segmented Transfer in Half-Duplex 1105 30.5.9.6 Configuration of Slave Segmented Transfer in Full-Duplex 1105 30.6 CS Setup Time and Hold Time Control 1106 30.7 GP-SPI Clock Control 1107 30.7.1 Clock Phase and Polarity 1108 30.7.2 Clock Control in Master Mode 1109 30.7.3 Clock Control in Slave Mode 1109 30.8 GP-SPI Timing Compensation 1110 30.9 Differences Between GP-SPI2 and GP-SPI3 1112 30.10 Interrupts 1113 30.11 Register Summary 1115 30.12 Registers 1116 

Espressif Systems 18 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

31 Two­wire Automotive Interface (TWAI®) 1149 31.1 Overview 1149 31.2 Features 1149 31.3 Functional Protocol 1149 

31.3.1 TWAI Properties 1149 31.3.2 TWAI Messages 1150 31.3.2.1 Data Frames and Remote Frames 1151 31.3.2.2 Error and Overload Frames 1153 31.3.2.3 Interframe Space 1154 31.3.3 TWAI Errors 1155 31.3.3.1 Error Types 1155 31.3.3.2 Error States 1155 31.3.3.3 Error Counters 1156 31.3.4 TWAI Bit Timing 1157 31.3.4.1 Nominal Bit 1157 31.3.4.2 Hard Synchronization and Resynchronization 1158 31.4 Architectural Overview 1158 31.4.1 Registers Block 1159 31.4.2 Bit Stream Processor 1160 31.4.3 Error Management Logic 1160 31.4.4 Bit Timing Logic 1160 31.4.5 Acceptance Filter 1160 31.4.6 Receive FIFO 1160 31.5 Functional Description 1161 31.5.1 Modes 1161 31.5.1.1 Reset Mode 1161 31.5.1.2 Operation Mode 1161 31.5.2 Bit Timing 1161 31.5.3 Interrupt Management 1162 31.5.3.1 Receive Interrupt (RXI) 1163 31.5.3.2 Transmit Interrupt (TXI) 1163 31.5.3.3 Error Warning Interrupt (EWI) 1163 31.5.3.4 Data Overrun Interrupt (DOI) 1163 31.5.3.5 Error Passive Interrupt (TXI) 1164 31.5.3.6 Arbitration Lost Interrupt (ALI) 1164 31.5.3.7 Bus Error Interrupt (BEI) 1164 31.5.3.8 Bus Status Interrupt (BSI) 1164 31.5.4 Transmit and Receive Buffers 1164 31.5.4.1 Overview of Buffers 1164 31.5.4.2 Frame Information 1165 31.5.4.3 Frame Identifier 1166 31.5.4.4 Frame Data 1167 31.5.5 Receive FIFO and Data Overruns 1167 31.5.5.1 Single Filter Mode 1168 31.5.5.2 Dual FIlter Mode 1168 31.5.6 Error Management 1169 

Espressif Systems 19 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

31.5.6.1 Error Warning Limit 1169 31.5.6.2 Error Passive 1171 31.5.6.3 Bus-Off and Bus-Off Recovery 1171 

31.5.7 Error Code Capture 1171 31.5.8 Arbitration Lost Capture 1172 31.6 Register Summary 1174 31.7 Registers 1175 

32 USB On­The­Go (USB) 1188 32.1 Overview 1188 32.2 Features 1188 

32.2.1 General Features 1188 32.2.2 Device Mode Features 1188 32.2.3 Host Mode Features 1188 

32.3 Functional Description 1189 32.3.1 Controller Core and Interfaces 1189 32.3.2 Memory Layout 1190 

32.3.2.1 Control & Status Registers 1191 32.3.2.2 FIFO Access 1191 32.3.3 FIFO and Queue Organization 1191 32.3.3.1 Host Mode FIFOs and Queues 1192 32.3.3.2 Device Mode FIFOs 1193 32.3.4 Interrupt Hierarchy 1193 32.3.5 DMA Modes and Slave Mode 1195 32.3.5.1 Slave Mode 1195 32.3.5.2 Buffer DMA Mode 1195 32.3.5.3 Scatter/Gather DMA Mode 1195 32.3.6 Transaction and Transfer Level Operation 1196 32.3.6.1 Transaction and Transfer Level in DMA Mode 1196 32.3.6.2 Transaction and Transfer Level in Slave Mode 1196 32.4 OTG 1198 32.4.1 OTG Interface 1198 32.4.2 ID Pin Detection 1199 32.4.3 Session Request Protocol (SRP) 1199 32.4.3.1 A-Device SRP 1199 32.4.3.2 B-Device SRP 1200 32.4.4 Host Negotiation Protocol (HNP) 1201 32.4.4.1 A-Device HNP 1201 32.4.4.2 B-Device HNP 1202 

33 USB Serial/JTAG Controller (USB\_SERIAL\_JTAG) 1204 33.1 Overview 1204 33.2 Features 1204 33.3 Functional Description 1206 

33.3.1 USB Serial/JTAG host connection 1206 33.3.2 CDC-ACM USB Interface Functional Description 1208 

Espressif Systems 20 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

33.3.3 CDC-ACM Firmware Interface Functional Description 1209 33.3.4 USB-to-JTAG Interface 1210 33.3.5 JTAG Command Processor 1210 33.3.6 USB-to-JTAG Interface: CMD\_REP usage example 1211 33.3.7 USB-to-JTAG Interface: Response Capture Unit 1211 33.3.8 USB-to-JTAG Interface: Control Transfer Requests 1212 

33.4 Recommended Operation 1213 33.4.1 Internal/external PHY selection 1213 33.4.2 Runtime operation 1214 

33.5 Register Summary 1216 33.6 Registers 1217 

34 SD/MMC Host Controller (SDHOST) 1230 34.1 Overview 1230 34.2 Features 1230 34.3 SD/MMC External Interface Signals 1230 34.4 Functional Description 1231 

34.4.1 SD/MMC Host Controller Architecture 1231 34.4.1.1 Bus Interface Unit (BIU) 1232 34.4.1.2 Card Interface Unit (CIU) 1232 

34.4.2 Command Path 1232 34.4.3 Data Path 1233 34.4.3.1 Data Transmit Operation 1233 34.4.3.2 Data Receive Operation 1234 34.5 Software Restrictions for Proper CIU Operation 1234 34.6 RAM for Receiving and Sending Data 1236 34.6.1 TX RAM Module 1236 34.6.2 RX RAM Module 1236 34.7 DMA Descriptor Chain 1236 34.8 The Structure of DMA descriptor chain 1236 34.9 Initialization 1239 34.9.1 DMA Initialization 1239 34.9.2 DMA Transmission Initialization 1239 34.9.3 DMA Reception Initialization 1240 34.10 Clock Phase Selection 1240 34.11 Interrupt 1241 34.12 Register Summary 1243 34.13 Registers 1245 

35 LED PWM Controller (LEDC) 1271 35.1 Overview 1271 35.2 Features 1271 35.3 Functional Description 1271 

35.3.1 Architecture 1271 35.3.2 Timers 1272 35.3.2.1 Clock Source 1272 

Espressif Systems 21 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

35.3.2.2 Clock Divider Configuration 1273 35.3.2.3 14-bit Counter 1274 35.3.3 PWM Generators 1275 35.3.4 Duty Cycle Fading 1276 35.3.5 Interrupts 1276 35.4 Register Summary 1277 35.5 Registers 1279 

36 Motor Control PWM (MCPWM) 1286 36.1 Overview 1286 36.2 Features 1286 36.3 Submodules 1288 

36.3.1 Overview 1288 36.3.1.1 Prescaler Submodule 1288 36.3.1.2 Timer Submodule 1288 36.3.1.3 Operator Submodule 1289 36.3.1.4 Fault Detection Submodule 1291 36.3.1.5 Capture Submodule 1291 

36.3.2 PWM Timer Submodule 1291 36.3.2.1 Configurations of the PWM Timer Submodule 1291 36.3.2.2 PWM Timer’s Working Modes and Timing Event Generation 1292 36.3.2.3 PWM Timer Shadow Register 1296 36.3.2.4 PWM Timer Synchronization and Phase Locking 1296 

36.3.3 PWM Operator Submodule 1296 36.3.3.1 PWM Generator Submodule 1297 36.3.3.2 Dead Time Generator Submodule 1307 36.3.3.3 PWM Carrier Submodule 1311 36.3.3.4 Fault Handler Submodule 1313 

36.3.4 Capture Submodule 1314 36.3.4.1 Introduction 1314 36.3.4.2 Capture Timer 1315 36.3.4.3 Capture Channel 1315 

36.4 Register Summary 1316 36.5 Registers 1319 

37 Remote Control Peripheral (RMT) 1370 37.1 Overview 1370 37.2 Features 1370 37.3 Functional Description 1370 

37.3.1 Architecture 1371 37.3.2 RAM 1371 37.3.2.1 RAM Architecture 1371 37.3.2.2 Use of RAM 1372 37.3.2.3 RAM Access 1373 37.3.3 Clock 1374 37.3.4 Transmitter 1374 

Espressif Systems 22 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

37.3.4.1 Normal TX Mode 1374 37.3.4.2 Wrap TX Mode 1375 37.3.4.3 TX Modulation 1375 37.3.4.4 Continuous TX Mode 1375 37.3.4.5 Simultaneous TX Mode 1376 

37.3.5 Receiver 1376 37.3.5.1 Normal RX Mode 1376 37.3.5.2 Wrap RX Mode 1376 37.3.5.3 RX Filtering 1377 37.3.5.4 RX Demodulation 1377 

37.3.6 Configuration Update 1377 37.4 Interrupts 1378 37.5 Register Summary 1378 37.6 Registers 1381 

38 Pulse Count Controller (PCNT) 1395 38.1 Features 1395 38.2 Functional Description 1396 38.3 Applications 1398 

38.3.1 Channel 0 Incrementing Independently 1398 38.3.2 Channel 0 Decrementing Independently 1399 38.3.3 Channel 0 and Channel 1 Incrementing Together 1399 

38.4 Register Summary 1401 38.5 Registers 1402 

39 On­Chip Sensors and Analog Signal Processing 1408 39.1 Overview 1408 39.2 Capacitive Touch Sensors 1408 

39.2.1 Terminology 1408 39.2.2 Overview 1408 39.2.3 Features 1409 39.2.4 Capacitive Touch Pins 1410 39.2.5 Touch Sensors Operating Principle and Signals 1411 39.2.6 Touch FSM 1412 

39.2.6.1 Measurement Process 1413 39.2.6.2 Measurement Trigger Source 1413 39.2.6.3 Scan Mode 1414 

39.2.7 Touch Detection 1415 39.2.7.1 Sampled Values 1415 39.2.7.2 Hardware Touch Detection 1415 

39.2.8 Noise Detection 1416 39.2.9 Proximity Mode 1417 39.2.10 Moisture Tolerance and Water Rejection 1417 

39.2.10.1 Moisture Tolerance 1418 39.2.10.2 Water Rejection 1418 39.3 SAR ADCs 1418 

Espressif Systems 23 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
Contents GoBack 

39.3.1 Overview 1418 39.3.2 Features 1419 39.3.3 SAR ADC Architecture 1420 39.3.4 Input Signals 1422 39.3.5 ADC Conversion and Attenuation 1422 39.3.6 RTC ADC Controller 1422 39.3.7 DIG ADC Controller 1423 

39.3.7.1 DIG ADC Clock 1424 39.3.7.2 DMA Support 1424 39.3.7.3 DIG ADC FSM 1424 39.3.7.4 Pattern Table 1424 39.3.7.5 Configuration Example for Multi-Channel Scanning 1426 39.3.7.6 DMA Data Format 1426 39.3.7.7 ADC Filters 1427 39.3.7.8 Threshold Monitoring 1427 

39.3.8 SAR ADC2 Arbiter 1427 39.4 Temperature Sensor 1428 39.4.1 Overview 1428 39.4.2 Features 1428 39.4.3 Functional Description 1429 39.5 Interrupts 1430 39.6 Register Summary 1430 39.6.1 SENSOR (ALWAYS\_ON) Register Summary 1430 39.6.2 SENSOR (RTC\_PERI) Register Summary 1431 39.6.3 SENSOR (DIG\_PERI) Register Summary 1432 39.7 Registers 1433 39.7.1 SENSOR (ALWAYS\_ON) Registers 1433 39.7.2 SENSOR (RTC\_PERI) Registers 1440 39.7.3 SENSOR (DIG\_PERI) Registers 1458 

40 Related Documentation and Resources 1469 

Glossary 1470 Abbreviations for Peripherals 1470 Abbreviations Related to Registers 1470 Access Types for Registers 1472 

Revision History 1474 

Espressif Systems 24 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Tables GoBack 

List of Tables 

1-1 Instruction Field Names and Descriptions 41 1-2 Register List of ESP32-S3 Extended Instruction Set 42 1-3 Data Format and Alignment 45 1-4 Extended Instruction List 48 1-5 Read Instructions 50 1-6 Write Instructions 51 1-7 Data Exchange Instructions 51 1-8 Vector Addition Instructions 52 1-9 Vector Multiplication Instructions 53 1-10 Vector Complex Multiplication Instructions 53 1-11 Vector Multiplication Accumulation Instructions 54 1-12 Vector and Scalar Multiplication Accumulation Instructions 56 1-13 Other Instructions 56 1-14 Comparison Instructions 56 1-15 Bitwise Logical Instructions 57 1-16 Shift Instructions 57 1-17 Butterfly Computation Instructions 58 1-18 Bit Reverse Instruction 58 1-19 Real Number FFT Instructions 59 1-20 GPIO Control Instructions 59 1-21 Five-Stage Pipeline of Xtensa Processor 61 1-22 Extended Instruction Pipeline Stages 62 2-1 Comparison of the Two Coprocessors 297 2-2 ALU Operations Among Registers 302 2-3 ALU Operations with Immediate Value 303 2-4 ALU Operations with Stage Count Register 303 2-5 Data Storage Type \- Automatic Storage Mode 305 2-6 Data Storage \- Manual Storage Mode 306 2-7 Input Signals Measured Using the ADC Instruction 310 2-8 Instruction Efficiency 312 2-9 ULP-RISC-V Interrupt Sources 313 2-10 ULP-RISC-V Interrupt Registers 314 2-11 ULP-RISC-V Interrupt List 316 2-12 Address Mapping 320 2-13 Description of Registers for Peripherals Accessible by ULP Coprocessors 320 3-1 Selecting Peripherals via Register Configuration 348 3-2 Descriptor Field Alignment Requirements for Accessing Internal RAM 351 3-3 Descriptor Field Alignment Requirements for Accessing External RAM 351 3-4 Relationship Between Configuration Register, Block Size and Alignment 352 4-1 Internal Memory Address Mapping 388 4-2 External Memory Address Mapping 390 4-3 Module/Peripheral Address Mapping 393 5-1 Parameters in eFuse BLOCK0 397 

Espressif Systems 25 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Tables GoBack 

5-2 Secure Key Purpose Values 400 5-3 Parameters in BLOCK1 to BLOCK10 401 5-4 Registers Information 406 5-5 Configuration of Default VDDQ Timing Parameters 408 6-1 Bits Used to Control IO MUX Functions in Light-sleep Mode 465 6-2 Peripheral Signals via GPIO Matrix 468 6-3 IO MUX Pin Functions 478 6-4 RTC Functions of RTC IO MUX Pins 479 6-5 Analog Functions of RTC IO MUX Pins 480 7-1 Reset Sources 511 7-2 CPU Clock Source 513 7-3 CPU Clock Frequency 513 7-4 Peripheral Clocks 514 7-5 APB\_CLK Fequency 515 7-6 CRYPTO\_PWM\_CLK Frequency 515 8-1 Default Configuration of Strapping Pins 516 8-2 Boot Mode Control 517 8-3 Control of ROM Messages Printing to UART0 518 8-4 Control of ROM Messages Printing to USB Serail/JTAG controller 518 8-5 JTAG Signal Source Control 519 9-1 CPU Peripheral Interrupt Configuration/Status Registers and Peripheral Interrupt Sources 522 9-2 CPU Interrupts 525 10-1 Low-Power Clocks 551 10-2 The Triggering Conditions for the RTC Timer 551 10-3 RTC Statues Transition 558 10-4 Predefined Power Modes 559 10-5 Wakeup Source 560 11-1 UNIT*n* Configuration Bits 615 11-2 Trigger Point 616 11-3 Synchronization Operation 616 12-1 Alarm Generation When Up-Down Counter Increments 635 12-2 Alarm Generation When Up-Down Counter Decrements 635 15-1 ROM Address 662 15-2 Access Configuration to ROM 662 15-3 SRAM Address 663 15-4 Internal SRAM0 Usage Configuration 664 15-5 Access Configuration to Internal SRAM0 664 15-6 Internal SRAM1 Split Regions 665 15-7 Access Configuration to the Instruction Region of Internal SRAM1 667 15-8 Access Configuration to the Data Region of Internal SRAM1 667 15-9 Internal SRAM2 Usage Configuration 669 15-10 Access Configuration to Internal SRAM2 669 15-11 RTC FAST Memory Address 669 15-12 Split RTC FAST Memory into the Higher Region and the Lower Region 670 15-13 Access Configuration to the RTC FAST Memory 670 15-14 RTC SLOW Memory Address 670 

Espressif Systems 26 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Tables GoBack 

15-15 Split RTCSlow\_0 and RTCSlow\_1 into Split Regions 671 15-16 Access Configuration to the RTC SLOW Memory 671 15-17 Access Configuration of the Peripherals 672 15-18 Access Configuration of Peri Regions 673 15-19 Split the External Memory into Split Regions 674 15-20 Access Configuration of External Memory Regions 675 15-21 Split the External SRAM into Four Split Regions for GDMA 675 15-22 Access Configuration of External SRAM via GDMA 676 15-23 Interrupt Registers for Unauthorized IBUS Access 677 15-24 Interrupt Registers for Unauthorized DBUS Access 677 15-25 Interrupt Registers for Unauthorized Access to External Memory 678 15-26 Interrupt Registers for Unauthorized Access to Internal Memory via GDMA 678 15-27 Interrupt Registers for Unauthorized PIF Access 679 15-28 All Possible Access Alignment and their Results 679 15-29 Interrupt Registers for Unauthorized Access Alignment 680 15-30 Lock Registers and Related Permission Control Registers 681 17-1 Internal Memory Controlling Bit 800 17-2 Peripheral Clock Gating and Reset Bits 802 18-1 SHA Accelerator Working Mode 820 18-2 SHA Hash Algorithm Selection 820 18-6 The Storage and Length of Message digest from Different Algorithms 826 19-1 AES Accelerator Working Mode 834 19-2 Key Length and Encryption / Decryption 834 19-3 Working Status under Typical AES Working Mode 834 19-4 Text Endianness Type for Typical AES 835 19-5 Key Endianness Type for AES-128 Encryption and Decryption 835 19-6 Key Endianness Type for AES-256 Encryption and Decryption 836 19-7 Block Cipher Mode 837 19-8 Working Status under DMA-AES Working mode 838 19-9 TEXT-PADDING 838 19-10 Text Endianness for DMA-AES 839 20-1 Acceleration Performance 850 20-2 RSA Accelerator Memory Blocks 850 21-1 HMAC Purposes and Configuration Values 857 23-1 *Key* generated based on *KeyA*, *KeyB* and *KeyC* 881 23-2 Mapping Between Offsets and Registers 882 26-1 UART*n* Synchronous Registers 906 26-2 UART*n* Static Registers 907 27-1 I2C Synchronous Registers 958 28-2 I2S*n* Signal Description 1008 28-3 Bit Width of Channel Valid Data 1014 

28-4 Endian of Channel Valid Data 1015 28-5 Data-Fetching Control in PDM Mode 1018 28-6 I2S*n* Channel Control in PDM Mode 1018 28-7 PCM-to-PDM Output Mode 1019 28-8 PDM-to-PCM Input Mode 1021 

Espressif Systems 27 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Tables GoBack 

28-9 Channel Storage Data Width 1022 28-10 Channel Storage Data Endian 1023 29-1 Signal Description 1044 29-2 LCD Data Format Control 1048 29-3 CAM Data Format Control 1049 29-4 Conversion Mode Control 1051 30-2 Data Modes Supported by GP-SPI2 and GP-SPI3 1074 30-3 Functional Description of FSPI/SPI3 Bus Signals 1075 30-4 FSPI bus Signals Used in Various SPI Modes 1076 30-5 SPI3 bus Signals Used in Various SPI Modes 1077 30-6 Bit Order Control in GP-SPI Master and Slave Modes 1079 30-7 Supported Transfers in Master and Slave Modes 1080 30-8 Interrupt Trigger Condition on GP-SPI Data Transfer in Slave Mode 1083 30-9 Registers Used for State Control in 1/2/4/8-bit Modes 1090 30-10 Sending Sequence of Command Value 1092 30-11 Sending Sequence of Address Value 1093 30-12 BM Table for CONF State 1098 30-13 An Example of CONF buffer*i* in Segment*i* 1099 30-14 BM Bit Value v.s. Register to Be Updated in This Example 1099 30-15 Supported CMD Values in SPI Mode 1102 30-15 Supported CMD Values in SPI Mode 1103 30-16 Supported CMD Values in QPI Mode 1103 30-17 Clock Phase and Polarity Configuration in Master Mode 1109 30-18 Clock Phase and Polarity Configuration in Slave Mode 1109 30-19 Invalid Registers and Fields for GP-SPI3 1112 30-20 GP-SPI Master Mode Interrupts 1114 30-21 GP-SPI Slave Mode Interrupts 1114 31-1 Data Frames and Remote Frames in SFF and EFF 1152 31-2 Error Frame 1153 31-3 Overload Frame 1154 31-4 Interframe Space 1154 31-5 Segments of a Nominal Bit Time 1157 31-6 Bit Information of TWAI\_BUS\_TIMING\_0\_REG (0x18) 1162 31-7 Bit Information of TWAI\_BUS\_TIMING\_1\_REG (0x1c) 1162 31-8 Buffer Layout for Standard Frame Format and Extended Frame Format 1164 31-9 TX/RX Frame Information (SFF/EFF)TWAI Address 0x40 1165 31-10 TX/RX Identifier 1 (SFF); TWAI Address 0x44 1166 31-11 TX/RX Identifier 2 (SFF); TWAI Address 0x48 1166 31-12 TX/RX Identifier 1 (EFF); TWAI Address 0x44 1166 31-13 TX/RX Identifier 2 (EFF); TWAI Address 0x48 1166 31-14 TX/RX Identifier 3 (EFF); TWAI Address 0x4c 1166 31-15 TX/RX Identifier 4 (EFF); TWAI Address 0x50 1166 31-16 Bit Information of TWAI\_ERR\_CODE\_CAP\_REG (0x30) 1171 31-17 Bit Information of Bits SEG.4 \- SEG.0 1172 31-18 Bit Information of TWAI\_ARB LOST CAP\_REG (0x2c) 1173 32-1 IN and OUT Transactions in Slave Mode 1197 

Espressif Systems 28 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Tables GoBack 

32-2 UTMI OTG Interface 1198 33-1 Standard CDC-ACM Control Requests 1208 33-2 CDC-ACM Settings with RTS and DTR 1209 33-3 Commands of a Nibble 1210 33-4 USB-to-JTAG Control Requests 1212 33-5 JTAG Capabilities Descriptor 1212 33-6 Use cases and eFuse settings 1213 33-7 IO Pad Status After Chip Initialization in the USB-OTG Download Mode 1213 33-8 Reset SoC into Download Mode 1214 33-9 Reset SoC into Booting 1215 34-1 SD/MMC Signal Description 1231 34-2 Word DES0 of SD/MMC GDMA Linked List 1237 34-3 Word DES1 of SD/MMC GDMA Linked List 1238 34-4 Word DES2 of SD/MMC GDMA Linked List 1238 34-5 Word DES3 of SD/MMC GDMA Linked List 1238 34-6 SDHOST Clk Phase Selection 1241 35-1 Commonly-used Frequencies and Resolutions 1274 36-1 Configuration Parameters of the Operator Submodule 1290 36-2 Timing Events Used in PWM Generator 1298 36-3 Timing Events Priority When PWM Timer Increments 1299 36-4 Timing Events Priority when PWM Timer Decrements 1299 36-5 Dead Time Generator Switches Control Fields 1308 36-6 Typical Dead Time Generator Operating Modes 1308 37-1 Configuration Update 1377 38-1 Counter Mode. Positive Edge of Input Pulse Signal. Control Signal in Low State 1397 38-2 Counter Mode. Positive Edge of Input Pulse Signal. Control Signal in High State 1397 38-3 Counter Mode. Negative Edge of Input Pulse Signal. Control Signal in Low State 1397 38-4 Counter Mode. Negative Edge of Input Pulse Signal. Control Signal in High State 1397 39-1 ESP32-S3 Capacitive Touch Pins 1410 39-2 Smooth Algorithm 1415 39-3 Benchmark Algorithm 1415 39-4 Noise Algorithm 1416 39-5 Hysteresis Algorithm 1416 39-6 SAR ADC Input Signals 1422 39-7 Temperature Offset 1430 

Espressif Systems 29 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Figures GoBack 

List of Figures 

1-1 PIE Internal Structure (MAC) 37 1-2 EE.ZERO.QACC in Little-Endian Bit Order 39 1-3 EE.ZERO.QACC in Big-Endian Bit Order 39 1-4 EE.ZERO.QACC in Little-Endian Byte Order 39 1-5 EE.ZERO.QACC in Big-Enidan Byte Order 40 1-6 Interlock Caused by Instruction Operand Dependency 62 1-7 Hardware Resource Hazard 70 1-8 Control Hazard 71 2-1 ULP Coprocessor Overview 296 2-2 ULP Coprocessor Diagram 297 2-3 Programming Workflow 298 2-4 ULP Sleep and Wakeup Sequence 299 2-5 Control of ULP Program Execution 300 2-6 ULP-FSM Instruction Format 301 2-7 Instruction Type — ALU for Operations Among Registers 301 2-8 Instruction Type — ALU for Operations with Immediate Value 302 2-9 Instruction Type — ALU for Operations with Stage Count Register 303 2-10 Instruction Type \- ST 303 2-11 Instruction Type \- Offset in Automatic Storage Mode (ST-OFFSET) 304 2-12 Instruction Type \- Data Storage in Automatic Storage Mode (ST-AUTO-DATA) 304 2-13 Data Structure of RTC\_SLOW\_MEM\[Rdst \+ Offset\] 305 2-14 Instruction Type \- Data Storage in Manual Storage Mode 305 2-15 Instruction Type \- LD 306 2-16 Instruction Type \- JUMP 307 2-17 Instruction Type \- JUMPR 307 2-18 Instruction Type \- JUMPS 308 2-19 Instruction Type \- HALT 309 2-20 Instruction Type \- WAKE 309 2-21 Instruction Type \- WAIT 309 2-22 Instruction Type \- TSENS 310 2-23 Instruction Type \- ADC 310 2-24 Instruction Type \- REG\_RD 311 2-25 Instruction Type \- REG\_WR 312 2-26 Standard R-type Instruction Format 314 2-27 Interrupt Instruction \- getq rd, qs 314 2-28 Interrupt Instruction \- setq qd,rs 315 2-29 Interrupt Instruction \- retirq 315 2-30 Interrupt Instruction — Maskirq rd rs 315 2-31 I2C Read Operation 318 2-32 I2C Write Operation 319 3-1 Modules with GDMA Feature and GDMA Channels 345 3-2 GDMA Engine Architecture 346 3-3 Structure of a Linked List 347 

Espressif Systems 30 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Figures GoBack 

3-4 Channel Buffer 349 3-5 Relationship among Linked Lists 350 3-6 Dividing External RAM into Areas 352 4-1 System Structure and Address Mapping 386 4-2 Cache Structure 391 4-3 Peripherals/modules that can work with GDMA 393 5-1 Shift Register Circuit (output of first 32 bytes) 404 5-2 Shift Register Circuit (output of last 12 bytes) 404 6-1 Architecture of IO MUX, RTC IO MUX, and GPIO Matrix 458 6-2 Internal Structure of a Pad 459 6-3 GPIO Input Synchronized on APB Clock Rising Edge or on Falling Edge 460 6-4 Filter Timing of GPIO Input Signals 460 7-1 Reset Levels 510 7-2 Clock Structure 512 9-1 Interrupt Matrix Structure 520 10-1 Low-power Management Schematics 547 10-2 Power Management Unit Workflow 549 10-3 RTC Clocks 550 10-4 Wireless Clocks 550 10-5 Digital Voltage Regulator 552 10-6 Low-power Voltage Regulator 553 10-7 Flash Voltage Regulator 553 10-8 Brown-out detector 554 10-9 Brown-out detector 555 10-10 RTC States 557 10-11 ESP32-S3 Boot Flow 563 11-1 System Timer Structure 613 11-2 System Timer Alarms 614 12-1 Timer Units within Groups 633 12-2 Timer Group Architecture 634 13-1 Watchdog Timers Overview 651 13-2 Watchdog Timers in ESP32-S3 653 13-3 Super Watchdog Controller Structure 656 14-1 XTAL32K Watchdog Timer 658 15-1 Split Lines for Internal SRAM1 665 15-2 An illustration of Configuring the Category fields 666 15-3 Three Ways to Access External Memory 674 16-1 Switching From Secure World to Non-secure World 781 16-2 Switching From Non-secure World to Secure World 782 16-3 World Switch Log Register 784 16-4 Nested Interrupts Handling \- Entry 9 785 16-5 Nested Interrupts Handling \- Entry 1 785 16-6 Nested Interrupts Handling \- Entry 4 786 21-1 HMAC SHA-256 Padding Diagram 860 21-2 HMAC Structure Schematic Diagram 860 22-1 Software Preparations and Hardware Working Process 871 

Espressif Systems 31 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Figures GoBack 

23-1 External Memory Encryption and Decryption Operation Settings 879 24-1 XTAL\_CLK Pulse Width 889 25-1 Noise Source 890 26-1 UART Structure 893 26-2 UART Controllers Sharing RAM 895 26-3 UART Controllers Division 896 26-4 The Timing Diagram of Weak UART Signals Along Falling Edges 897 26-5 Structure of UART Data Frame 898 26-6 AT\_CMD Character Structure 898 26-7 Driver Control Diagram in RS485 Mode 899 26-8 The Timing Diagram of Encoding and Decoding in SIR mode 900 26-9 IrDA Encoding and Decoding Diagram 901 26-10 Hardware Flow Control Diagram 902 26-11 Connection between Hardware Flow Control Signals 902 26-12 Data Transfer in GDMA Mode 904 26-13 UART Programming Procedures 908 27-1 I2C Master Architecture 954 27-2 I2C Slave Architecture 954 27-3 I2C Protocol Timing (Cited from Fig.31 in The I2C-bus specification Version 2.1) 955 27-4 I2C Timing Parameters (Cited from Table 5 in The I2C-bus specification Version 2.1) 956 27-5 I2C Timing Diagram 959 27-6 Structure of I2C Command Registers 961 27-7 I2Cmaster Writing to I2Cslave with a 7-bit Address 964 27-8 I2Cmaster Writing to a Slave with a 10-bit Address 966 27-9 I2Cmaster Writing to I2Cslave with Two 7-bit Addresses 968 27-10 I2Cmaster Writing to I2Cslave with a 7-bit Address in Multiple Sequences 970 27-11 I2Cmaster Reading I2Cslave with a 7-bit Address 972 27-12 I2Cmaster Reading I2Cslave with a 10-bit Address 974 27-13 I2Cmaster Reading N Bytes of Data from addrM of I2Cslave with a 7-bit Address 976 27-14 I2Cmaster Reading I2Cslave with a 7-bit Address in Segments 979 28-1 ESP32-S3 I2S System Diagram 1007 28-2 TDM Philips Standard Timing Diagram 1009 

28-3 TDM MSB Alignment Standard Timing Diagram 1010 28-4 TDM PCM Standard Timing Diagram 1010 28-5 PDM Standard Timing Diagram 1011 28-6 I2S*n* Clock 1011 28-7 TX Data Format Control 1016 28-8 TDM Channel Control 1017 28-9 PDM Channel Control 1020 29-1 LCD\_CAM Block Diagram 1044 29-2 LCD Clock 1045 29-3 Camera Clock 1046 29-4 LCD Frame Structure 1052 29-5 LCD Timing (RGB Format) 1053 29-6 LCD Timing (I8080 Format) 1054 30-1 SPI Module Overview 1074 

Espressif Systems 32 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Figures GoBack 

30-2 Data Buffer Used in CPU-Controlled Transfer 1080 30-3 GP-SPI Block Diagram 1084 30-4 Data Flow Control in GP-SPI Master Mode 1085 30-5 Data Flow Control in GP-SPI Slave Mode 1085 30-6 GP-SPI State Machine in Master Mode 1088 30-7 Full-Duplex Communication Between GP-SPI2 Master and a Slave 1093 30-8 Connection of GP-SPI2 to Flash and External RAM in 4-bit Mode 1096 30-9 SPI Quad I/O Read Command Sequence Sent by GP-SPI2 to Flash 1096 30-10 Configurable Segmented Transfer in DMA-Controlled Master Mode 1097 30-11 Recommended CS Timing and Settings When Accessing External RAM 1106 30-12 Recommended CS Timing and Settings When Accessing Flash 1107 30-13 SPI Clock Mode 0 or 2 1108 30-14 SPI Clock Mode 1 or 3 1108 30-15 Timing Compensation Control Diagram in GP-SPI2 Master Mode 1110 30-16 Timing Compensation Example in GP-SPI2 Master Mode 1111 31-1 Bit Fields in Data Frames and Remote Frames 1151 31-2 Fields of an Error Frame 1153 31-3 Fields of an Overload Frame 1154 31-4 The Fields within an Interframe Space 1156 31-5 Layout of a Bit 1157 31-6 TWAI Overview Diagram 1159 31-7 Acceptance Filter 1168 31-8 Single Filter Mode 1169 31-9 Dual Filter Mode 1170 31-10 Error State Transition 1170 31-11 Positions of Arbitration Lost Bits 1173 32-1 OTG\_FS System Architecture 1189 32-2 OTG\_FS Register Layout 1190 32-3 Host Mode FIFOs 1192 32-4 Device Mode FIFOs 1193 32-5 OTG\_FS Interrupt Hierarchy 1194 32-6 Scatter/Gather DMA Descriptor List 1195 32-7 A-Device SRP 1200 32-8 B-Device SRP 1200 32-9 A-Device HNP 1201 32-10 B-Device HNP 1202 33-1 USB Serial/JTAG High Level Diagram 1205 33-2 USB Serial/JTAG Block Diagram 1206 33-3 USB Serial/JTAG and USB-OTG Internal/External PHY Routing Diagram 1207 33-4 JTAG Routing Diagram 1208 34-1 SD/MMC Controller Topology 1230 34-2 SD/MMC Controller External Interface Signals 1231 34-3 SDIO Host Block Diagram 1232 34-4 Command Path State Machine 1233 34-5 Data Transmit State Machine 1234 34-6 Data Receive State Machine 1234 

Espressif Systems 33 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Figures GoBack 

34-7 Descriptor Chain 1236 34-8 The Structure of a Linked List 1237 34-9 Clock Phase Selection 1241 35-1 LED PWM Architecture 1271 35-2 LED PWM Generator Diagram 1272 35-3 Frequency Division When LEDC\_CLK\_DIV is a Non-Integer Value 1273 35-4 LED\_PWM Output Signal Diagram 1275 35-5 Output Signal Diagram of Fading Duty Cycle 1276 36-1 MCPWM Module Overview 1286 36-2 Prescaler Submodule 1288 36-3 Timer Submodule 1288 36-4 Operator Submodule 1289 36-5 Fault Detection Submodule 1291 36-6 Capture Submodule 1291 36-7 Count-Up Mode Waveform 1292 36-8 Count-Down Mode Waveforms 1293 36-9 Count-Up-Down Mode Waveforms, Count-Down at Synchronization Event 1293 36-10 Count-Up-Down Mode Waveforms, Count-Up at Synchronization Event 1293 36-11 UTEP and UTEZ Generation in Count-Up Mode 1294 36-12 DTEP and DTEZ Generation in Count-Down Mode 1295 36-13 DTEP and UTEZ Generation in Count-Up-Down Mode 1295 36-14 Submodules Inside the PWM Operator 1297 36-15 Symmetrical Waveform in Count-Up-Down Mode 1300 36-16 Count-Up, Single Edge Asymmetric Waveform, with Independent Modulation on PWM*x*A and 

PWM*x*B — Active High 1301 36-17 Count-Up, Pulse Placement Asymmetric Waveform with Independent Modulation on PWM*x*A 1302 36-18 Count-Up-Down, Dual Edge Symmetric Waveform, with Independent Modulation on PWM*x*A and 

PWM*x*B — Active High 1303 36-19 Count-Up-Down, Dual Edge Symmetric Waveform, with Independent Modulation on PWM*x*A and PWM*x*B — Complementary 1304 36-20 Example of an NCI Software-Force Event on PWM*x*A 1305 36-21 Example of a CNTU Software-Force Event on PWM*x*B 1306 36-22 Options for Setting up the Dead Time Generator Submodule 1308 36-23 Active High Complementary (AHC) Dead Time Waveforms 1309 36-24 Active Low Complementary (ALC) Dead Time Waveforms 1309 36-25 Active High (AH) Dead Time Waveforms 1310 36-26 Active Low (AL) Dead Time Waveforms 1310 36-27 Example of Waveforms Showing PWM Carrier Action 1311 36-28 Example of the First Pulse and the Subsequent Sustaining Pulses of the PWM Carrier Submodule 1312 36-29 Possible Duty Cycle Settings for Sustaining Pulses in the PWM Carrier Submodule 1313 37-1 RMT Architecture 1371 37-2 Format of Pulse Code in RAM 1372 38-1 PCNT Block Diagram 1395 38-2 PCNT Unit Architecture 1396 38-3 Channel 0 Up Counting Diagram 1398 38-4 Channel 0 Down Counting Diagram 1399 

Espressif Systems 34 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
List of Figures GoBack 

38-5 Two Channels Up Counting Diagram 1399 39-1 Touch Sensor 1409 39-2 Touch Sensor Operating Principle 1411 39-3 Touch Sensor Structure 1411 39-4 Touch FSM Structure 1412 39-5 Timing Diagram of Touch Scan 1414 39-6 Sensing Area 1417 39-7 SAR ADC Overview 1419 39-8 SAR ADC Architecture 1420 39-9 RTC ADC Controller Overview 1423 39-10 APB\_SARADC\_SAR1\_PATT\_TAB1\_REG and Pattern Table Entry 0 \- Entry 3 1425 39-11 APB\_SARADC\_SAR1\_PATT\_TAB2\_REG and Pattern Table Entry 4 \- Entry 7 1425 39-12 APB\_SARADC\_SAR1\_PATT\_TAB3\_REG and Pattern Table Entry 8 \- Entry 11 1425 39-13 APB\_SARADC\_SAR1\_PATT\_TAB4\_REG and Pattern Table Entry 12 \- Entry 15 1425 39-14 Pattern Table Entry 1425 39-15 SAR ADC1 cmd0 Configuration 1426 39-16 SAR ADC1 cmd1 Configuration 1426 39-17 DMA Data Format 1426 39-18 Temperature Sensor Overview 1429 

Espressif Systems 35 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1 Processor Instruction Extensions (PIE) 

1.1 Overview 

The ESP32-S3 adds a series of extended instruction set in order to improve the operation efficiency of specific AI and DSP (Digital Signal Processing) algorithms. This instruction set is designed from the TIE (Tensilica Instruction Extension) language, and adds general-purpose registers with large bit width, various special registers and processor ports. Based on the SIMD (Single Instruction Multiple Data) concept, this instruction set supports 8-bit, 16-bit, and 32-bit vector operations, which greatly increases data operation efficiency. In addition, the arithmetic instructions, such as multiplication, shifting, and accumulation, can perform data operations and transfer data at the same time, thus further increasing execution efficiency of a single instruction. 

1.2 Features 

The PIE (Processor Instruction Extensions) has the following features: 

• 128-bit general-purpose registers 

• 128-bit vector operations, e.g., multiplication, addition, subtraction, accumulation, shifting, comparison, etc. 

• Integration of data transfer into arithmetic instructions 

• Support for non-aligned 128-bit vector data 

• Support for saturation operation 

1.3 Structure Overview 

A structure overview should help to understand list of available instructions, instructions possibilities, and limits. It is not intended to describe hardware details. 

The internal structure of PIE for multiplication-accumulation (MAC) instructions overview could be described as shown below: 

Espressif Systems 36 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

|  |
| :---- |

Figure 1­1. PIE Internal Structure (MAC) 

The diagram above shows the data flow paths and PIE components. 

The PIE unit contains: 

• Address unit that reads 8/16/32/64/128-bit aligned data 

• Bank of eight 128-bit vector QR registers 

• Arithmetic logic unit (ALU) with 

– sixteen 8-bit multipliers 

– eight 16-bit multipliers 

• QACC\_H/QACC\_L \- 2 160-bit accumulators 

• ACCX \- 40-bit accumulator 

1.3.1 Bank of Vector Registers 

Bank of vector registers contains 8 vector registers (QR). Each register could be represented as an array of 16 x 8-bit data words, array of 8 x 16-bit data words, or array of 4 x 32-bit data words. Depending on the used instructions, 8, 16 or 32-bit data format will be chosen. 

Espressif Systems 37 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.3.2 ALU 

Arithmetic logic unit (ALU) could work for 8-bit input data, as 8-bit ALU, for 16-bit input data, as 16-bit ALU, or for 32-bit input data, as 32-bit ALU. 8-bit multiplication ALU contains 16 multipliers and is able to make up to 16 multiplications and accumulation in one instruction. With multiplication almost any other combinations of arithmetic operation are possible. For example, FFT instructions include multiplication, addition, and subtraction operations in one instruction. Also, ALU includes logic operations like AND, OR, shift, and so on. The input for ALU operation comes from QR registers. The result of operations could be saved to the QR registers or special accumulator registers (ACCX, QACC). 

1.3.3 QACC Accumulator Register 

The QACC accumulator register is used for multiplication-accumulation operations on 8-bit or 16-bit data. In the case of 8-bit data, QACC consists of 16 accumulator registers with 20-bit width. In the case of 16-bit data, QACC consists of 8 accumulator registers with 40-bit width. The following description reflects the case of 8-bit arithmetic. For 16-bit arithmetic, the logic is similar. 

After multiplication and accumulation on two vector QR registers, the result of 16 operations will be written to 16 20-bit accumulator registers. 

QACC is divided into two parts: 160-bit QACC\_H and 160-bit QACC\_L. The former stores the higher 160-bit data of QACC, and the latter stores the lower 160-bit data. To store the accumulator result in QR registers, it is possible to convert 20-bit result numbers to 8 bits by right-shifting it. For 16-bit multiplication-accumulation operation, convert the 40-bit result to 16-bit by right-shifting it. 

It is possible to load data from memory to QACC or reset the initial value to 0\. 

1.3.4 ACCX Accumulator Register 

Some operations require accumulating the result of all multipliers to one value. In this case, the ACCX accumulator should be used. 

ACCX is a 40-bit accumulator register. The result of the accumulators could be shifted and stored in the memory as an 8-bit or 16-bit value. 

It is possible to load data from memory to ACCX or reset the initial value to 0\. 

1.3.5 Address Unit 

Most of the instructions in PIE allow loading or storing data from/to 128-bit Q registers in parallel in one cycle. In most cases, the data should be 128-bit aligned, and the lower 4 bits of address will be ignored. The Address unit provides functionality to manipulate address registers in parallel, which saves the time to update address registers. 

It is possible to make address register operations like AR \+ signed constant, ARx \+ ARy, and AR \+ 16\. The Address unit makes post-processing operations. It means that all operations with address registers are done after instructions are finished. 

1.4 Syntax Description 

This section provides introduction to the encoding order of instructions and the meaning of characters that appear in the instruction descriptions. 

Espressif Systems 38 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.4.1 Bit/Byte Order 

The encoding order of instructions is divided into two types based on the granularity, i.e., bit order and byte order. According to the located side of the least bit or byte, there are big-endian order and little-endian order. That is to say, the most common encoding types for instructions are: little-endian bit order, big-endian bit order, little-endian byte order and big-endian byte order. 

• Little-endian bit order: the instruction is encoded in bit order, with the least significant bit on the right. • Big-endian bit order: the instruction is encoded in bit order, with the least significant bit on the left. • Little-endian byte order: the instruction is encoded in byte order, with the least significant byte on the right. • Big-endian byte order: the instruction is encoded in byte order, with the least significant byte on the left. 

Among them, the instruction encoding bit sequences obtained using little-endian byte order and little-endian bit order are identical. Taking the 24-bit instruction EE.ZERO.QACC as an example, Figure 1-2, Figure 1-3, Figure 1-4 and Figure 1-5 show the code of this instruction in little-endian bit order, big-endian bit order, little-endian byte order and big-endian byte order, respectively. 

Please note that all instructions and register descriptions appear in this chapter use little-endian bit order, which means the least significant bit is stored in the lowest addresses. 

|  |
| :---- |

Figure 1­2. EE.ZERO.QACC in Little­Endian Bit Order 

|  |
| :---- |

Figure 1­3. EE.ZERO.QACC in Big­Endian Bit Order 

|  |
| :---- |

Figure 1­4. EE.ZERO.QACC in Little­Endian Byte Order 

Espressif Systems 39 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

|  |
| :---- |

Figure 1­5. EE.ZERO.QACC in Big­Enidan Byte Order 

1.4.2 Instruction Field Definition 

Table 1-1 provides the meaning of the characters covered in instruction descriptions. You can find such characters and corresponding descriptions in Section 1.8. 

Espressif Systems 40   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Table 1­1. Instruction Field Names and Descriptions 

| Name  |  |  | Description |
| :---: | :---: | :---: | :---- |
| a\*  | 32-bit general-purpose registers | as | In-out type (used as input/output operand). Stores address information for read/write operations, which is updated after such operations are completed. |
|  |  | at | In-out type. Temporarily stores operation results to the EE.FFT.AMS.S16.ST.INCP instruction, which will be part of the data to be written to memory. |
|  |  | ad | In type (used as input operand). Stores data used to update address information. |
|  |  | av  | In type. Stores data to be written to memory. |
|  |  | ax,ay | In type. Stores data involved in arithmetic operations, e.g., shifting amounts, multipliers and etc. |
|  |  | au | Out type (used as output operand). Stores results of instruction operations. |
| q\*  | 128-bit general-purpose registers | qs | In type. Stores 128-bit data used for concatenation operations. |
|  |  | qa,qx,qy,qm  | In type. Stores data used for vector operations. |
|  |  | qz  | Out type. Stores results of vector operations. |
|  |  | qu  | Out type. Stores data read from memory. |
|  |  | qv  | In type. Stores data to be written to memory. |
| fu |  |  | 32-bit general-purpose floating-point register. Stores floating-point data read from memory. |
| fv |  |  | 32-bit general-purpose floating-point register. Stores floating-point data to be written to memory. |
| sel2 |  |  | 1-bit immediate value ranging from 0 to 1\. Used to select signals. |
| sel4,upd4 |  |  | 2-bit immediate value ranging from 0 to 3\. Used to select signals. |
| sel8 |  |  | 3-bit immediate value ranging from 0 to 7\. Used to select signals. |
| sel16 |  |  | 4-bit immediate value ranging from 0 to 15\. Used to select signals. |
| sar2 |  |  | 1-bit immediate value ranging from 0 to 1\. Represents shifting numbers. |
| sar4 |  |  | 2-bit immediate value ranging from 0 to 3\. Represents shifting numbers. |
| sar16 |  |  | 4-bit immediate value ranging from 0 to 15\. Represents shifting numbers. |
| imm1 |  |  | 7-bit unsigned immediate value ranging from 0 to 127 with an interval of 1\. This is used to show the size of the updated read/write operation address value. |

Cont’d on next page 

Espressif Systems 41   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Table 1­1 – cont’d from previous page 

| Name  | Description |
| :---: | :---- |
| imm2 | 7-bit unsigned immediate value ranging from 0 to 254 with an interval of 2\. This is used to show the size of the updated read/write operation address value. |
| imm4 | 8-bit signed immediate value ranging from \-256 to 252 with an interval of 8\. This is used to show the size of the updated read/write operation address value. |
| imm16 | 8-bit signed immediate value ranging from \-2048 to 2032 with an interval of 16\. This is used to show the size of the updated read/write operation address value. |
| imm16f | 4-bit signed immediate value ranging from \-128 to 112 with an interval of 16\. This is used to show the size of the updated read/write operation address value. |

Some instructions have multiple operands with the same function. Those operands are distinguished by adding numbers after field names. For example, the EE.LDF.128.IP instruction has four fu registers, fu0 \~ 3\. They are used to store 128-bit data read from memory. 

1.5 Components of Extended Instruction Set 

1.5.1 Registers 

This section introduces all kinds of registers related to ESP32-S3’s extended instruction set, including the original registers defined by Xtensa as well as customized registers. For register information, please refer to Table 1-2. 

Table 1­2. Register List of ESP32­S3 Extended Instruction Set 

| Register Mnemonics  | Quantity  | Bit Width  | Access  | Type |
| ----- | ----- | :---: | :---: | ----- |
| AR  | 161  | 32  | R/W  | General-purpose registers |
| FR  | 16  | 32  | R/W  | General-purpose registers to FPU |
| QR  | 8  | 128  | R/W  | Customized general-purpose registers |
| SAR  | 1  | 6  | R/W  | Special register |
| SAR\_BYTE  | 1  | 4  | R/W  | Customized special register |
| ACCX  | 1  | 40  | R/W  | Customized special register |
| QACC\_H  | 1  | 160  | R/W  | Customized special register |
| QACC\_L  | 1  | 160  | R/W  | Customized special register |
| FFT\_BIT\_WIDTH  | 1  | 4  | R/W  | Customized special register |
| UA\_STATE  | 1  | 128  | R/W  | Customized special register |

1 The Xtensa processor has 64 internal AR registers. It is designed with the register windowing tech nique, so that the software can only access 16 of the 64 AR registers at any given time. The pro gramming performance can be effectively improved by rotating windows, replacing function calls, and saving registers when exceptions are triggered. 

Espressif Systems 42 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.5.1.1 General­Purpose Registers 

When using general-purpose register as operands in instructions, you need to explicitly declare the number of the assigned register. For example: 

*EE.V ADDS.S*8 *q*2*, q*0*, q*1 

This instruction uses No.0 and No.1 QR registers as input vectors and stores the vector addition result in the No.2 QR register. 

AR 

Each AR register operand in the instruction will occupy a 4-bit code length. You can select any of the 16 AR registers as operands, and the 4-bit code value indicates the number to declare. The row ”a\*” in table 1-1 lists various purposes of AR registers in the extended instruction set, including address storage and data storage. 

FR 

Each FR register operand in the instruction will occupy a 4-bit code length. You can select any of the 16 FR registers as operands, and the 4-bit code value indicates the number to declare. In ESP32-S3 extended instruction set, there are only read and write instructions for floating-point data. They are 4 times more efficient than the 32-bit floating-point data R/W instructions that are native to the Xtensa processor, thanks to the 128-bit access bandwidth. 

QR 

In order to improve the execution efficiency of the program, operands are usually stored in general-purpose registers to save time spent in reading from memory. The AR registers native to Xtensa only have 32-bit width, while ESP32-S3 can access 128-bit data at a time, so they can only use 1/4 bandwidth capacity of the existing data bus. For this reason, ESP32-S3 has added eight 128-bit customized general-purpose registers, i.e., QR registers. QR registers are mainly used to store the data acquired/used by the 128-bit data bus to read or write memory, as well as to temporarily store the operation results generated from 128-bit data operations. 

As the processor executes instructions, an individual QR register is treated as 16 8-bit or 8 16-bit or 4 32-bit operands depending on the vector operation bit width defined by the instruction, thus enabling a single instruction to perform operations on multiple operands. 

1.5.1.2 Special Registers 

Different from general-purpose registers, special registers are implicitly called in specific instructions. You do not need to and cannot specify a certain special register when executing instructions. For example: 

*EE.V MUL.S*16 *q*2*, q*0*, q*1 

This vector multiplication instruction uses q0 and q1 general-purpose registers as inputs. During the internal operation, the intermediate 32-bit multiplication result is shifted to the right, and then the lower 16-bit of the result is retained to form a 128-bit output to q2. The shift amount in the process is determined by the value in the Shift Amount Register (SAR) and this SAR register will not appear in the instruction operand list. 

Espressif Systems 43 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

SAR 

The Shift Amount Register (SAR) stores the shift value in bits. There are two types of instructions in ESP32-S3’s extended instruction set that use SAR. One is the type of instructions to shift vector data, including EE.VSR.32 and EE.VSL.32. The former uses the lower 5 bits of SAR as the right-shift value, and the latter uses the lower 5 

bits of SAR as the left-shift value. The other type is multiplication instructions, including EE.VMUL.\*, EE.CMUL.\*, EE.FFT.AMS.\* and EE.FFT.CMUL.\*. This type of instructions uses the value in SAR as the value for the right shift of the intermediate multiplication result, which determines the accuracy of the final result. 

SAR\_BYTE 

The SAR\_BYTE stores the shift value in bytes. This special register is designed to handle the non-aligned 128-bit data (see Section 1.5.3). For vector arithmetic instructions, the data read or stored by extended instructions are forced to be 16-byte aligned, but in practice, there is no guarantee that the data addresses used are always 16-byte aligned. 

EE.LD.128.USAR.IP and EE.LD.128.USAR.XP instructions write the lower 4-bit values of the memory access register that represent non-aligned data to SAR\_BYTE while reading 128-bit data from memory. 

There are two types of instruction in ESP32-S3’s extended instruction set that use SAR\_BYTE. One is dedicated to handling non-aligned data in QR registers, including EE.SRCQ.\* and EE.SRC.Q\*. This type of instruction will read two 16-byte data from two aligned addresses, before and after the non-aligned address, put them together, and then shift it by the byte size of SAR\_BYTE to get a 128-bit data from the non-aligned address. The other type of instruction handles non-aligned data while executing arithmetic operations, which usually has a suffix of ”.QUP”. 

ACCX 

Multiplier-accumulator. Instructions such as EE.VMULAS.\*.ACCX\* and EE.SRS.ACCX use this register during operations. The former uses ACCX to accumulate all vector multiplication results of two QR registers, and the latter right shifts the ACCX register. 

QACC\_H,QACC\_L 

Successive accumulators partitioned by segments. Instructions such as EE.VMULAS.\*.QACC\* and EE.SRCMB.\*.QACC use this type of registers during operations. These registers are mainly used to accumulate vector multiplication results of two QR registers into the corresponding segments of QACC\_H and QACC\_L respectively. The 16-bit vector multiplication results are accumulated into the corresponding 16 20-bit segments respectively and the 32-bit results are accumulated into the corresponding 8 40-bit segement respectively. 

FFT\_BIT\_WIDTH 

This special register is dedicated to the EE.BITREV instruction. The value inside this register is used to indicate the operating mode of EE.BITREV. Its range is 0 \~ 7, indicating 3-bit \~ 10-bit operating mode respectively. For more details, please refer to instruction EE.BITREV. 

UA\_STATE 

This special register is dedicated to the EE.FFT.AMS.S16.LD.INCP.UAUP instruction. This register is used to store the non-aligned 128-bit data read from memory. Next time when this instruction is called, the data in this 

Espressif Systems 44 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

register is concatenated to the newly read non-aligned data and then the result is shifted to obtain the 128-bit aligned data. 

1.5.2 Fast GPIO Interface 

ESP32-S3’s Xtensa processor adds two signal ports, i.e., GPIO\_OUT and GPIO\_IN. You can route signals from the two ports to specified GPIO pins via the GPIO Matrix. 

1.5.2.1 GPIO\_OUT 

An 8-bit processor output interface. Firstly, configure the 8-bit port signals to specified pins via GPIO Matrix. For core0, their names are pro\_alonegpio\_out0\~7\. For core1, their names are core1\_gpio\_out0\~7\. Then you can set certain bits of GPIO\_OUT to 1 via instructions EE.WR\_MASK\_GPIO\_OUT and EE.SET\_BIT\_GPIO\_OUT, or set certain bits to 0 via instruction EE.CLR\_BIT\_GPIO\_OUT, so as to pull certain pins to high level or low level. Using this method, you can get faster response than pulling pins through register configurations. 

1.5.2.2 GPIO\_IN 

An 8-bit processor input interface. Firstly, configure the 8-bit port signals to specified pins via GPIO Matrix. For core0, their names are pro\_alonegpio\_in0\~7\. For core1, their names are core1\_gpio\_in0\~7\. Then you can read the eight GPIO pin levels and store them to the AR register through instruction EE.GET\_GPIO\_IN. Using this method, you can get and handle the level changes on GPIO pins faster than reading registers to get pin level status. 

1.5.3 Data Format and Alignment 

The current extended instruction set supports 1-byte, 2-byte, 4-byte, 8-byte and 16-byte data formats. Besides, there is also a 20-byte format: QACC\_H and QACC\_L. However, there is no direct way to switch the data between the two special registers and memory. You can read and write data of QACC\_H and QACC\_L via five 4-byte (AR) registers or two 16-byte (QR) registers. 

The table 1-3 lists bit length and alignment information for common data format (’x’ indicates that the bit is either 0 and 1). The Xtensa processor uses byte as the smallest unit for addresses stored in memory in all data formats. And little-endian byte order is used, with byte 0 stored in the lowest bit (the right side), as shown in Figure 1-4. 

Table 1­3. Data Format and Alignment 

| Data Format  | Length  | Aligned Addresses in Memory |
| ----- | :---: | :---: |
| 1-byte  | 8 bits  | xxxx |
| 2-byte  | 16 bits  | xxx0 |
| 4-byte  | 32 bits  | xx00 |
| 8-byte  | 64 bits  | x000 |
| 16-byte  | 128 bit  | 0000 |

However, if data is stored in memory at a non-aligned address, direct access to this address may cause it being split into two accesses, which in turn affects the performance of the code. For example, if you expect to read a 16-byte data from memory, as shown in Table 1-3, the data is stored in memory at 0000 when the data is 

Espressif Systems 45 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

aligned. But actually the data is not aligned, so the low nibble of its address may be any one between 0000 \~ 1111 (binary). Assuming the lowest bit of its address is 0\_0100, the processor will split the one-time access to this data into two accesses, i.e., to 0\_0000 and 1\_0000 respectively. The processor then put together the obtained two 16-byte data to get the required 16-byte data. 

To avoid performance degradation caused by the above non-aligned access operations, all access addresses in the extended instruction set are forced to be aligned, i.e., the lowest bits will be replaced by 0\. For example, if a read operation is initiated for 128-bit data at 0x3fc8\_0024, the lowest 4-bit of this access address will be forced to be set to 0\. Eventually, the 128-bit data stored at 0x3fc8\_0020 will be read. Similarly, the lowest 3-bit of the 

access address for 64-bit data will be set to 0; the lowest 2-bit of the access address for 32-bit data will be set to 0; the lowest 1-bit of the access address for 16-bit data will be set to 0\. 

The above design requires aligned addresses of the access operations initiated. Otherwise, the data read will not be what you expected. In application code, you need to explicitly declare the alignment of the variable or array in memory. 16-byte alignment can meet the needs of most application scenarios. 

The *aligned*(16) parameter declares that the variable is stored in a 16-byte aligned memory address. You can also request a data space with its starting address 16-byte aligned via heap\_caps\_aligned\_alloc. 

Since the memory address of the data involved in some operations is uncertain in specific application scenarios, this extended instruction set provides a special register SAR\_BYTE and related instructions such as EE.LD.128.USAR.\* and EE.SRC.\*, to handle non-aligned data. 

Assume that the 128-bit non-aligned data address is stored in the general-purpose register a8. This 128-bit data can be read into the specified QR register (q2 in the following example) by the following code: *EE.LD.*128*.USAR.IP q*0*, a*8*,* 16 

*EE.V LD.*128*.IP q*1*, a*8*,* 16 

*EE.SRC.Q q*2*, q*0*, q*1 

1.5.4 Data Overflow and Saturation Handling 

Data overflow means that the size of the operation result exceeds the maximum value that can be stored in the result register. Take the EE.VMUL.S8 instruction as an example, the result of two 8-bit multipliers is 16-bit, and it should still be 16-bit after right-shifting. However, the final result will be stored in the 8-bit register, which may cause the risk of data overflow. 

In the design of the ESP32-S3’s instruction extensions, there are two ways to handle data overflow, namely taking saturation and truncating the least significant bit. The former controls the calculation result according the range of values that can be stored in the result register. If the result exceeds the maximum value of the result register, take the maximum value; if the result is smaller than the minimum value of the result register, take the minimum value. This approach will be explicitly indicated in the instruction descriptions. For example, the EE.VADDS.\* instructions perform saturation to the results of addition operations. Regarding the data overflow handling for more instructions of their internal calculation results, the wraparound approach is used, i.e., only the lower bit of the result that is consistent with the bit width of the result register will be retained and stored in the result register. 

Please note that for instructions that do not mention saturation handling method, the wraparound approach will be used. 

Espressif Systems 46 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.6 Extended Instruction List 

Table 1-4 lists instruction types and corresponding instruction information included in the extended instruction set. This section gives brief introduction to all types of instructions. 

Espressif Systems 47   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Table 1­4. Extended Instruction List 

| Instruction Type  | Instruction1 | Reference  Section |
| :---- | :---- | :---- |
| Read instructions | EE.VLD.128.\[XP/IP\] | 1.6.1 |
|  | EE.VLD.\[H/L\].64.\[XP/IP\] |  |
|  | EE.VLDBC.\[8/16/32\].\[-/XP/IP\] |  |
|  | EE.VLDHBC.16.INCP |  |
|  | EE.LDF.\[64/128\].\[XP/IP\] |  |
|  | EE.LD.128.USAR.\[XP/IP\] |  |
|  | EE.LDQA.\[U/S\]\[8/16\].128.\[XP/IP\] |  |
|  | EE.LD.QACC\_\[H/L\].\[H.32/L.128\].IP |  |
|  | EE.LD.ACCX.IP |  |
|  | EE.LD.UA\_STATE.IP |  |
|  | EE.LDXQ.32 |  |
| Write instructions | EE.VST.128.\[XP/IP\] | 1.6.2 |
|  | EE.VST.\[H/L\].64.\[XP/IP\] |  |
|  | EE.STF.\[64/128\].\[XP/IP\] |  |
|  | EE.ST.QACC\_\[H/L\].\[H.32/L.128\].IP |  |
|  | EE.ST.ACCX.IP |  |
|  | EE.ST.UA\_STATE.IP |  |
|  | EE.STXQ.32 |  |
| Arithmetic instructions | EE.VADDS.S\[8/16/32\].\[-/LD.INCP/ST.INCP\] | 1.6.4 |
|  | EE.VSUBS.S\[8/16/32\].\[-/LD.INCP/ST.INCP\] |  |
|  | EE.VMUL.\[U/S\]\[8/16\].\[-/LD.INCP/ST.INCP\] |  |
|  | EE.CMUL.S16.\[-/LD.INCP/ST.INCP\] |  |
|  | EE.VMULAS.\[U/S\]\[8/16\].ACCX.\[-/LD.IP/LD.XP\] |  |
|  | EE.VMULAS.\[U/S\]\[8/16\].QACC.\[-/LD.IP/LD.XP/LDBC.INCP\] |  |
|  | EE.VMULAS.\[U/S\]\[8/16\].ACCX.\[LD.IP/LD.XP\].QUP |  |
|  | EE.VMULAS.\[U/S\]\[8/16\].QACC.\[LD.IP/LD.XP/LDBC.INCP\].QUP |  |
|  | EE.VSMULAS.S\[8/16\].QACC.\[-/LD.INCP\] |  |
|  | EE.SRCMB.S\[8/16\].QACC |  |
|  | EE.SRS.ACCX |  |
|  | EE.VRELU.S\[8/16\] |  |
|  | EE.VPRELU.S\[8/16\] |  |
| Comparison instructions | EE.VMAX.S\[8/16/32\].\[-/LD.INCP/ST.INCP\] | 1.6.5 |
|  | EE.VMIN.S\[8/16/32\].\[-/LD.INCP/ST.INCP\]  |  |
|  | EE.VCMP.\[EQ/LT/GT\].S\[8/16/32\] |  |
| Bitwise logic instructions | EE.ORQ | 1.6.6 |
|  | EE.XORQ |  |
|  | EE.ANDQ |  |
|  | EE.NOTQ |  |

Con’t on next page 

Espressif Systems 48   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Table1­4 – con’t from previous page 

| Instruction Type  | Instruction1 | Reference  Section |
| ----- | ----- | :---- |
| Shift instructions | EE.SRC.Q | 1.6.7 |
|  | EE.SRC.Q.QUP |  |
|  | EE.SRC.Q.LD.\[XP/IP\] |  |
|  | EE.SLCI.2Q |  |
|  | EE.SLCXXP.2Q |  |
|  | EE.SRCI.2Q |  |
|  | EE.SRCXXP.2Q |  |
|  | EE.SRCQ.128.ST.INCP |  |
|  | EE.VSR.32 |  |
|  | EE.VSL.32 |  |
| FFT dedicated instructions | EE.FFT.R2BF.S16.\[-/ST.INCP\] | 1.6.8  |
|  | EE.FFT.CMUL.S16.\[LD.XP/ST.XP\] |  |
|  | EE.BITREV |  |
|  | EE.FFT.AMS.S16.\[LD.INCP.UAUP/LD.INCP/LD.R32.DECP/ST.INCP\] |  |
|  | EE.FFT.VST.R32.DECP |  |
| GPIO control instructions | EE.WR\_MASK\_GPIO\_OUT | 1.6.9 |
|  | EE.SET\_BIT\_GPIO\_OUT |  |
|  | EE.CLR\_BIT\_GPIO\_OUT |  |
|  | EE.GET\_GPIO\_IN |  |
| Processor control instructions | RSR.\* | 1.6.10 |
|  | WSR.\* |  |
|  | XSR.\* |  |
|  | RUR.\* |  |
|  | WUR.\* |  |

1 For detailed information of these instructions, please refer to Section 1.8. 

1.6.1 Read Instructions 

Read instructions tell the processor to issue a virtual address to access memory based on the AR register that stores access address information. Most read instructions read memory first, and then update the access address. EE.LDXQ.32 is a special case where the instruction first selects a piece of 16-bit data in the QR register via an immediate value, adds it to the access address, and then issues the access operation. 

Since access to non-aligned addresses will cause slower response, all virtual addresses issued by read instructions in the extended instruction set are forced to be aligned according to data formats. Depending on the size of the access data format, corresponding length of data will be returned by memory as 1-byte, 2-byte, 4-byte, 8-byte or 16-byte. When the data read after forced alignment is not as expected, the desired data can be extracted from multiple QR registers using instructions such as EE.SRC.Q. 

The table below briefly describes the access operations performed by read instructions. For detailed information about read instructions, please see Section 1.8. 

Espressif Systems 49 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

Table 1­5. Read Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.VLD.128.XP | Read the 16-byte data, then add the value stored in the AR register to the access address. |
| EE.VLD.128.IP  | Read the 16-byte data, then add the immediate value to the access address. |
| EE.VLD.\[H/L\].64.XP | Read the 8-byte data, then add the value stored in the AR register to the access address. |
| EE.VLD.\[H/L\].64.IP  | Read the 8-byte data, then add the immediate value to the access address. |
| EE.VLDBC.\[8/16/32\]  | Read the 1-byte/2-byte/4-byte data. |
| EE.VLDBC.\[8/16/32\].XP | Read the 1-byte/2-byte/4-byte data, then add the value stored in the AR register to the access address. |
| EE.VLDBC.\[8/16/32\].IP | Read the 1-byte/2-byte/4-byte data, then add the immediate value to the access address. |
| EE.VLDHBC.16.INCP  | Read the 16-byte data, then add 16 to the access address. |
| EE.LDF.\[64/128\].XP | Read the 8-byte/16-byte data, then add the value stored in the AR register to the access address. |
| EE.LDF.\[64/128\].IP | Read the 8-byte/16-byte data, then add the immediate value to the access ad dress. |
| EE.LD.128.USAR.XP | Read the 16-byte data, then add the value stored in the AR register to the access address. |
| EE.LD.128.USAR.IP  | Read the 16-byte data, then add the immediate value to the access address. |
| EE.LDQA.U8.128.\[XP/IP\] | Read the 16-byte data and slice it by 1-byte, and zero-extend it to 20-bit data, which then will be written to register QACC\_H and QACC\_L, and finally add the value stored in the AR register or the immediate value to the access address. |
| EE.LDQA.U16.128.XP | Read the 16-byte data and slice it by 2-byte, and zero-extend it to 40-bit data, which then will be written to register QACC\_H and QACC\_L, and finally add the value stored in the AR register or the immediate value to the access address. |
| EE.LDQA.S8.128.XP | Read the 16-byte data and slice it by 1-byte, then sign-extend it to 20-bit data, which then will be written to register QACC\_H and QACC\_L, and finally add the value stored in the AR register or the immediate value to the access address. |
| EE.LDQA.S16.128.XP | Read the 16-byte data and slice it by 1-byte, then sign-extend it to 40-bit data, which then will be written to register QACC\_H and QACC\_L, and finally add the value stored in the AR register or the immediate value to the access address. |
| EE.LD.QACC\_\[H/L\].H.32.IP  | Read the 4-byte data, then add the immediate value to the access address. |
| EE.LD.QACC\_\[H/L\].L.128.IP  | Read the 16-byte data, then add the immediate value to the access address. |
| EE.LD.ACCX.IP  | Read the 8-byte data, then add the immediate value to the access address. |
| EE.LD.UA\_STATE.IP  | Read the 16-byte data, then add the immediate value to the access address. |
| EE.LDXQ.32  | Update access address first, then read the 4-byte data. |

1.6.2 Write Instructions 

The write instructions tells the processor to issue a virtual address to access memory based on the AR register that stores the information about access addresses. Most write instructions write memory first then update the access addresses, except for the EE.STXQ.32 instruction, which selects a piece of 16-bit data first from the QR register via an immediate value, adds it to the access address, and then issues the access operation. 

Espressif Systems 50   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Since access to non-aligned addresses will cause slower response, all virtual addresses issued by write instructions in the extended instruction set are forced to be aligned according to data format. Depending on the size of the access data format, corresponding length of data will be written to memory as 1-byte, 2-byte, 4-byte, 8-byte or 16-byte. When the data length to be written to memory is less than the access bit length, it is necessary to perform zero extension or sign extension on this data. 

The table below briefly describes the access operations performed by write instructions. For detailed information, please refer to Section 1.8. 

Table 1­6. Write Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.VST.128.XP | Write the 16-byte data to memory, then add the value stored in the AR register to the access address. |
| EE.VST.128.IP | Write the 16-byte data to memory, then add the immediate value to the access address. |
| EE.VST.\[H/L\].64.XP | Write the 8-byte data to memory, then add the value stored in the AR register to the access address. |
| EE.VST.\[H/L\].64.IP | Write the 8-byte data to memory, then add the immediate value to the access address. |
| EE.STF.\[64/128\].XP | Write the 8-byte/16-byte data to memory, then add the value stored in the AR register to the access address. |
| EE.STF.\[64/128\].IP | Write the 8-byte/16-byte data to memory, then add the immediate value to the access address. |
| EE.ST.QACC\_\[H/L\].H.32.IP | Write the 4-byte data to memory, then add the immediate value to the access address. |
| EE.ST.QACC\_\[H/L\].L.128.IP | Write the 16-byte data to memory, then add the immediate value to the access address. |
| EE.ST.ACCX.IP | Zero-extend the value in the ACCX register to 8-byte data and write it to memory, then add the immediate value to the access address. |
| EE.ST.UA\_STATE.IP | Write the 16-byte data to memory, then add the immediate value to the access address. |
| EE.STXQ.32  | Update the access address first, then write the 4-byte data to memory. |

1.6.3 Data Exchange Instructions 

Data exchange instructions are mainly used to exchange data information between different registers. Considering the bit width of the exchange registers are different, the immediate value are added as the selection signal, and zero extension and sign extension instructions are provided also. A variety of data exchange instructions can meet the data exchange requirements for users under various scenarios. 

For detailed information about data exchange instructions, please refer to Section 1.8. Table 1­7. Data Exchange Instructions 

| Instructions  | Description |
| :---- | :---- |
| EE.MOVI.32.A  | Assign a piece of 32-bit data from the QR register to the AR register. |

Espressif Systems 51 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

Instructions Description 

| EE.MOVI.32.Q | Assign the data stored in the AR register to a piece of 32-bit data space in the QR register. |
| ----- | :---- |
| EE.VZIP.\[8/16/32\]  | Encoding the two QR registers in 1-byte/2-byte/4-byte unit. |
| EE.VUNZIP.\[8/16/32\]  | Decoding the two QR registers in 1-byte/2-byte/4-byte unit. |
| EE.ZERO.Q  | Clear a specified QR register. |
| EE.ZERO.QACC  | Clear QACC\_H and QACC\_L registers. |
| EE.ZERO.ACCX  | Clear a specified ACCX register. |
| EE.MOV.S8.QACC | Slice the QR register by 1-byte, and sign-extend it to 20-bit data, then assign this value to QACC\_H and QACC\_L registers. |
| EE.MOV.S16.QACC | Slice the QR register by 2-byte, and sign-extend it to 40-bit data, then assign this value to QACC\_H and QACC\_L registers. |
| EE.MOV.U8.QACC | Slice the QR register by 1-byte, and zero-extend it to 20-bit, then assign this value to QACC\_H and QACC\_L registers. |
| EE.MOV.U16.QACC | Slice the QR register by 2-byte, and zero-extend it to 40-bit data, then assign this value to QACC\_H and QACC\_L registers. |

1.6.4 Arithmetic Instructions 

Arithmetic instructions mainly use the SIMD (Single Instruction Multiple Data) principle for vector data operations, including vector addition, vector multiplication, vector complex multiplication, vector multiplication accumulation, vector and scalar multiplication accumulation, etc. 

Vector Addition Instructions 

ESP32-S3 provides vector addition and subtraction instructions for data in 1-byte, 2-byte and 4-byte units. 

Considering that the input and output operands required for vector operations are stored in memory, in order to reduce extra operations as reading memory and to improve the speed of code execution, vector addition instructions are designed to perform the addition and the 16-byte access at the same time, and the value in the address register is increased by 16 after the access, thus directly pointing to the next continuous 16-byte memory address. You can select the appropriate instruction according to the actual algorithm needs. 

In addition, vector addition instructions also saturate the result of addition and subtraction to ensure the accuracy of arithmetic operations. 

Table 1­8. Vector Addition Instructions 

| Instructions  | Description |
| ----- | ----- |
| EE.VADDS.S\[8/16/32\]  | Perform vector addition operation on 1-byte/2-byte/4-byte data. |
| EE.VADDS.S\[8/16/32\].LD.INCP | Perform vector addition operation on 1-byte/2-byte/4-byte data, and read 16-byte data from memory at the same time. |
| EE.VADDS.S\[8/16/32\].ST.INCP | Perform vector addition operation on 1-byte/2-byte/4-byte data, and write 16-byte data to memory at the same time. |
| EE.VSUBS.S\[8/16/32\]  | Perform vector subtraction operation on 1-byte/2-byte/4-byte data. |
| EE.VSUBS.S\[8/16/32\].LD.INCP | Perform vector subtraction operation on 1-byte/2-byte/4-byte data, and read 16-byte data from memory at the same time. |

Espressif Systems 52 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

Instructions Description 

| EE.VSUBS.S\[8/16/32\].ST.INCP | Perform vector subtraction operation on 1-byte/2-byte/4-byte data, and write 16-byte data to memory at the same time. |
| :---: | :---- |

Vector Multiplication Instructions 

ESP32-S3 provides vector multiplication instructions for data in 1-byte and 2-byte units, and supports unsigned and signed vector multiplication. 

Considering that the input and output operands required for vector operations are stored in memory, in order to reduce extra operations as reading memory and improve the speed of code execution, vector multiplication instructions are designed to perform the multiplication and access 16 bytes at the same time, and the access address is increased by 16 after the access, thus directly pointing to the next 16-byte memory address. You can select the appropriate instruction according to the actual algorithm needs. 

Table 1­9. Vector Multiplication Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.VMUL.U\[8/16\]  | Perform vector multiplication operation on unsigned 1-byte/2-byte data. |
| EE.VMUL.S\[8/16\]  | Perform vector multiplication operation on signed 1-byte/2-byte data. |
| EE.VMUL.U\[8/16\].LD.INCP | Perform vector multiplication operation on unsigned 1-byte/2-byte data, and read 16-byte data from memory at the same time. |
| EE.VMUL.S\[8/16\].LD.INCP | Perform vector multiplication operation on signed 1-byte/2-byte data, and read 16-byte data from memory at the same time. |
| EE.VMUL.U\[8/16\].ST.INCP | Perform vector multiplication operation on unsigned 1-byte/2-byte data, and write 16-byte data to memory at the same time. |
| EE.VMUL.S\[8/16\].ST.INCP | Perform vector multiplication operation on signed 1-byte/2-byte data, and write 16-byte data to memory at the same time. |

Vector Complex Multiplication Instructions 

ESP32-S3 provides vector complex multiplication instructions for data in 2-byte unit. The instruction operands are executed as signed data. 

Considering that the input and output operands required for vector operations are stored in memory, in order to reduce extra operations as reading memory and improve the speed of code execution, vector complex multiplication instructions are designed to perform the multiplication and access 16 bytes at the same time, and the access address is increased by 16 after the access, thus directly pointing to the next 16-byte memory address. You can select the appropriate instruction according to the actual algorithm needs. 

Table 1­10. Vector Complex Multiplication Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.CMUL.S16  | Perform vector complex multiplication operation to 2-byte data. |
| EE.CMUL.S16.LD.INCP | Perform vector complex multiplication operation to 2-byte data, and read 16-byte data from memory at the same time. |
| EE.CMUL.S16.ST.INCP | Perform vector complex multiplication operation to 2-byte data, and write 16-byte data to memory at the same time. |

Espressif Systems 53 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

Vector Multiplication Accumulation Instructions 

ESP32-S3 provides two types of vector multiplication accumulation instructions: one is based on the ACCX register, accumulating multiple vector multiplication results to a 40-bit ACCX register; and the other is based on QACC\_H and QACC\_L registers, accumulating vector multiplication results to the corresponding bit segments of QACC\_H and QACC\_L registers respectively. Both types of above-mentioned instructions support multiplication accumulation on 1-byte and 2-byte segments. 

In order to reduce extra operations as reading memory and improve the speed of code execution, vector multiplication accumulation instructions are designed to perform the multiplication accumulation and access 16 bytes at the same time, and the access address is increased by the value in the AR register or by the immediate value after the access. 

In addition, instructions with the ”QUP” suffix in the vector multiplication accumulation instructions also support extracting 16-byte aligned data from the unaligned address. 

Table 1­11. Vector Multiplication Accumulation Instructions 

| Instructions  | Description |
| :---- | :---- |
| EE.VMULAS.\[U/S\]\[8/16\].ACCX | Perform vector multiplication accumulation to signed/un signed data in 1-byte/2-byte segment, and store the result to the ACCX register temporarily. |
| EE.VMULAS.\[U/S\]\[8/16\].ACCX.LD.IP | Perform vector multiplication accumulation to signed/un signed data in 1-byte/2-byte segment, and store the result to the ACCX register temporarily, then read 16-byte data from memory. Add immediate to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].ACCX.LD.XP | Perform vector multiplication accumulation to signed/un signed data in 1-byte/2-byte segment, and store the result to the ACCX register temporarily, then read 16-byte data from memory. Add the value of AR register to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].ACCX.LD.IP.QUP | Perform vector multiplication accumulation to signed/un signed data in 1-byte/2-byte segment, and store the result to the ACCX register temporarily. Then read 16-byte data from memory and output a 16-byte aligned data. Add immediate to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].ACCX.LD.XP.QUP | Perform vector multiplication accumulation to signed/un signed data in 1-byte/2-byte segment, and store the result to the ACCX register temporarily. Then read 16-byte data from memory and output a 16-byte aligned data. Add the value of AR register to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].QACC | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to corresponding bit segments in QACC\_H and QACC\_L registers. |

Espressif Systems 54   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Instructions Description 

| EE.VMULAS.\[U/S\]\[8/16\].QACC.LD.IP | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to corresponding bit segments in QACC\_H and QACC\_L registers, then read 16-byte data from memory. Add imme diate to address register. |
| ----- | :---- |
| EE.VMULAS.\[U/S\]\[8/16\].QACC.LD.XP | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to corresponding bit segments in QACC\_H and QACC\_L registers, then read 16-byte data from memory. Add the value of AR register to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].QACC.LDBC.INCP | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to corresponding bit segments in QACC\_H and QACC\_L registers. Then read 1-byte/2-byte data from memory and broadcast it to the 128-bit QR register. Add 16 to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].QACC.LD.IP.QUP | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to the corresponding bit segments in QACC\_H and QACC\_L registers. At the same time, read 16-byte data from mem ory and output a 16-byte aligned data. Add immediate to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].QACC.LD.XP.QUP | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to the corresponding bit segments in QACC\_H and QACC\_L registers. At the same time, read 16-byte data from mem ory and output a 16-byte aligned data. Add the value of AR register to address register. |
| EE.VMULAS.\[U/S\]\[8/16\].QACC.LDBC.INCP.QUP | Perform vector multiplication operation on signed/unsigned data in 1-byte/2-byte segment, and accumulate the results to the corresponding bit segments in QACC\_H and QACC\_L registers. At the same time, read 1-byte/2-byte data from memory and broadcast it to the 128-bit QR register, then output a 16-byte aligned data. Add 16 to address register. |

Vector and Scalar Multiplication Accumulation Instructions 

The function of this type of instructions is similar to that of the vector multiplication accumulation instructions based on QACC\_H and QACC\_L registers, except that one of the binocular operands is a vector and the other is a scalar. It also contains instructions that can execute access operation while vector operations are performed. 

Espressif Systems 55   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Table 1­12. Vector and Scalar Multiplication Accumulation Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.VSMULAS.S\[8/16\].QACC | Perform vector and scalar multiplication accumulation on signed data in 1-byte/2-byte segment. |
| EE.VSMULAS.S\[8/16\].QACC.LD.INCP | Perform vector and scalar multiplication accumulation on signed data in 1-byte/2-byte segment, and read 16-byte data from mem ory at the same time. Add 16 to address register. |

Other Instructions 

This section contains instructions that perform arithmetic right-shifting on multiplication accumulation results in QACC\_H, QACC\_L and ACCX. You can set the shifting value to obtain the multiplication accumulation results within the expected accuracy range. 

In addition, it also contains vector multiplication instructions with enabling conditions. 

Table 1­13. Other Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.SRCMB.S\[8/16\].QACC | Perform signed right-shifting on data in QACC\_H and QACC\_L registers in segment unit. |
| EE.SRS.ACCX  | Perform signed right-shifting on data in the ACCX register. |
| EE.VRELU.S\[8/16\]  | Perform vector and scalar multiplication based on enabling conditions. |
| EE.VPRELU.S\[8/16\]  | Perform vector-to-vector multiplication based on enabling conditions. |

1.6.5 Comparison Instructions 

Vector comparison instructions compare data in the unit of 1 byte, 2 bytes, or 4 bytes, including the instructions that take the larger/smaller one between the compared two values, that set all bits to 1 when the two values are equal and set them to 0 when they are not equal, that set all bits to 1 when the former value is larger than the latter and otherwise set them to 0, and that set all bits to 1 when the former value is smaller than the latter and otherwise set them to 0\. 

Considering that the input and output operands required for vector operations are stored in memory, in order to reduce extra operations as reading memory and improve the speed of code execution, access instructions to 16-byte addresses are performed at the same time as vector operations, and the access address is increased by 16 after the access, thus directly pointing to the next 16-byte memory address. You can select the appropriate instruction according to the actual algorithm needs. 

Table 1­14. Comparison Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.VMAX.S\[8/16/32\]  | Take the larger value between the two 1-byte/2-byte/4-byte values. |
| EE.VMAX.S\[8/16/32\].LD.INCP | Take the larger value between the two 1-byte/2-byte/4-byte values, and read 16-byte data from memory at the same time. |
| EE.VMAX.S\[8/16/32\].ST.INCP | Take the larger value between the two 1-byte/2-byte/4-byte values, and write 16-byte data to memory at the same time. |

Espressif Systems 56 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

Instructions Description 

| EE.VMIN.S\[8/16/32\]  | Take the smaller value between the two 1-byte/2-byte/4-byte values. |
| ----- | :---- |
| EE.VMIN.S\[8/16/32\].LD.INCP | Take the smaller value between the two 1-byte/2-byte/4-byte values, and read 16-byte data from memory at the same time. |
| EE.VMIN.S\[8/16/32\].ST.INCP | Take the smaller value between the two 1-byte/2-byte/4-byte values, and write 16-byte data to memory at the same time. |
| EE.VCMP.EQ.S\[8/16/32\] | Compare two 1-byte/2-byte/4-byte values, set all bits to 1 when the two values are equal, or set all bits to 0 when they are not equal. |
| EE.VCMP.LT.S\[8/16/32\] | Compare two 1-byte/2-byte/4-byte values, set all bits to 1 when the former value is smaller than the latter one, or set them to 0 otherwise. |
| EE.VCMP.GT.S\[8/16/32\] | Compare two 1-byte/2-byte/4-byte values, set all bits to 1 when the former value is larger than the latter one, or set them to 0 otherwise. |

1.6.6 Bitwise Logical Instructions 

Bitwise logical instructions include bitwise logical OR, bitwise logical AND, bitwise logical XOR and bitwise NOT instructions. 

Table 1­15. Bitwise Logical Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.ORQ  | Bitwise logical OR *qa* \= *qx|qy* |
| EE.XORQ  | Bitwise logical XOR *qa* \= *qx* ^ *qy* |
| EE.ANDQ  | Bitwise logical AND *qa* \= *qx*&*qy* |
| EE.NOTQ  | Bitwise NOT*qa* \= \~*qx* |

1.6.7 Shift Instructions 

Shift instructions include vector left-shift and vector right-shift instructions in 4-byte processing units as well as left-shift and right-shift instructions for spliced 16-byte data. The shift value of the former type is determined by the SAR register; while the shift value of the latter type can be determined by the SAR\_BYTE register, the immediate value, or the lower bits in the AR register. You can select appropriate instructions based on you application needs. 

All the shift instructions mentioned above are performed based on signed bits. 

Table 1­16. Shift Instructions 

| Instructions  | Description |
| :---- | :---- |
| EE.SRC.Q | Perform logical right-shift on the spliced 16-byte data, and the shift value is determined by the SAR\_BYTE register. |
| EE.SRC.Q.QUP | Perform logical right-shift on the spliced 16-byte data, and the shift value is determined by the SAR\_BYTE register. Meanwhile, the higher 8-byte data is saved. |
| EE.SRC.Q.LD.XP | Perform logical right-shift on the spliced 16-byte data, and the shift value is determined by the SAR\_BYTE register. At the same time, read 16-byte data from memory and add a register value to the read address. |

Espressif Systems 57   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Instructions Description 

| EE.SRC.Q.LD.IP | Perform logical right-shift on the spliced 16-byte data, and the shift value is determined by the SAR\_BYTE register. At the same time, read 16-byte data from memory and add an immediate number to the read address. |
| ----- | :---- |
| EE.SLCI.2Q | Perform logical left-shift on the spliced 16-byte data, and the shift value is determined by the immediate value. |
| EE.SLCXXP.2Q | Perform logical left-shift on the spliced 16-byte data, and the shift value is determined by the value in the AR register. |
| EE.SRCI.2Q | Perform logical right-shift on the spliced 16-byte data, and the shift value is determined by the immediate value. |
| EE.SRCXXP.2Q | Perform logical right-shift on the spliced 16-byte data, and the shift value is determined by the value in the AR register. |
| EE.SRCQ.128.ST.INCP | Perform logical right-shift on the spliced 16-byte data, which will be written to memory after the shift. |
| EE.VSR.32  | Perform vector arithmetic right-shift on the 4-byte data. |
| EE.VSL.32  | Perform vector arithmetic left-shift on the 4-byte data. |

1.6.8 FFT Dedicated Instructions 

FFT (Fast Fourier Transform) dedicated instructions include butterfly computation instructions, bit reverse instruction, and real number FFT instructions. 

Butterfly Computation Instructions 

Butterfly computation instructions support radix-2 butterfly computation. 

Table 1­17. Butterfly Computation Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.FFT.R2BF.S16.  | Perform radix-2 butterfly computation. |
| EE.FFT.R2BF.S16.ST.INCP | Perform radix-2 butterfly computation, and write the 16-byte result to memory at the same time. |
| EE.FFT.CMUL.S16.LD.XP | Perform radix-2 complex butterfly computation, and read 16-byte data from memory at the same time. |
| EE.FFT.CMUL.S16.ST.XP | Perform radix-2 complex butterfly computation, and write the 16- byte data (consists of the result and partial data segments in QR register) to memory at the same time. |

Bit Reverse Instruction 

The reverse bit width of this instruction is determined by the value in the FFT\_BIT\_WIDTH register. Table 1­18. Bit Reverse Instruction 

| Instruction  | Description |
| :---: | :---- |
| EE.BITREV  | Bit reverse instruction. |

Espressif Systems 58   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

Real Number FFT Instructions 

A single real number FFT instruction can perform a series of complex calculations including addition, multiplication, shifting, etc. 

Table 1­19. Real Number FFT Instructions 

| Instructions  | Description |
| ----- | :---- |
| EE.FFT.AMS.S16.LD.INCP.UAUP | Perform complex calculations and read 16-byte data from memory at the same time, then output 16-byte aligned data. |
| EE.FFT.AMS.S16.LD.INCP | Perform complex calculations and read 16-byte data from memory at the same time. Add 16 to address register. |
| EE.FFT.AMS.S16.LD.R32.DECP | Perform complex calculations and read 16-byte data from memory at the same time. Reverse the word order of read data. Add 16 to address register. |
| EE.FFT.AMS.S16.ST.INCP | Perform complex calculation and write 16-byte data (consists of the data in AR and partial data segments in QR) to memory at the same time. |
| EE.FFT.VST.R32.DECP | Splice the QR register in 2-byte unit, shift the result and write this 16-byte data to memory. |

1.6.9 GPIO Control Instructions 

GPIO control instructions include instructions to drive GPIO\_OUT and get the status of GPIO\_IN. Table 1­20. GPIO Control Instructions 

| Instruction  | Description |
| ----- | :---- |
| EE.WR\_MASK\_GPIO\_OUT  | Set GPIO\_OUT by mask. |
| EE.SET\_BIT\_GPIO\_OUT  | Set GPIO\_OUT. |
| EE.CLR\_BIT\_GPIO\_OUT  | Clear GPIO\_OUT. |
| EE.GET\_GPIO\_IN  | Get the status of GPIO\_IN. |

1.6.10 Processor Control Instructions 

As illustrated in Section 1.5.1.2, there are various special registers inside the ESP32-S3 processor. In order to facilitate the read and write of the values in such special registers, the following types of processor control instructions are provided to realize the data transfer between the special registers and the AR registers. 

RSR.\*Read Special register 

Can read the value from special registers that come with the processor to the AR register. “*∗*” stands for special registers, which only include the SAR register. 

WSR.\*Write Special register 

Can modify the value in special registers that come with the processor via the AR register. “*∗*” stands for special registers, which only include the SAR register. 

XSR.\*Exchange Special register 

Can exchange the values inside the AR register and special registers. “*∗*” stands for special registers, which only 

Espressif Systems 59 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

include the SAR register. 

RUR.\*Read User­defined register 

Can read the value from user-defined special registers in the processor to the AR register. “*∗*” stands for special registers, which include SAR\_BYTE, ACCX, QACC\_H, QACC\_L, FFT\_BIT\_WIDTH and UA\_STATE registers. 

WSR.\*Write User­defined register 

Can modify the value in user-defined special registers via the AR register. “*∗*” stands for special registers, which include SAR\_BYTE, ACCX, QACC\_H, QACC\_L, FFT\_BIT\_WIDTH and UA\_STATE registers. For special registers that exceed 32-bit width, the ”\_n” suffix is used to distinguish the instructions that read or write different 32-bit segments from the same special register. Taking reading data from the ACCX register as an example, there are two RUR.\* instructions, namely RUR.ACCX\_0 and RUR.ACCX\_1. The former reads the lower 32-bit data from the ACCX register and write it to the AR register; the latter read the left higher 8-bit data from the ACCX register, perform zero extension and write the result to the AR register. Accordingly, QACC\_H and QACC\_L registers realize data transfer via the five AR registers. 

Espressif Systems 60 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.7 Instruction Performance 

For processors designed based on a pipeline, it is ideal that CPU issues one instruction onto the pipeline per processor cycle. The ESP32-S3 Xtensa processor adopts the 5-stage pipeline technology: I (instruction fetch), R (decode), E (execute), M (memory access), and W (write back). Table 1-21 shows what the processor does at each pipeline stage. 

Table 1­21. Five­Stage Pipeline of Xtensa Processor 

| Pipeline Stage  | Number  | Operation |
| ----- | :---: | :---- |
| I  | \-  | Align instructions (24-bit and 32-bit instructions supported) |
| R  | 0 | Read the general-purpose registers AR and QR  Decode instructions, detect interlocks, and forward operands |
| E  | 1 | For arithmetic instructions, the ALU (addition, subtraction, multiplication, etc.) works For read memory instructions, generate virtual addresses for memory access For branch jump instructions, select jump addresses |
| M  | 2  | Issue read and write memory accesses |
| W  | 3  | Write back to registers the calculated results and the data read from memory |

The processor cannot issue an instruction to the pipeline until all the operands and hardware resources required for the operation are ready. However, there are the following hazards in the actual program running process, which can cause stopped pipeline and delayed implementation of instructions. 

1.7.1 Data Hazard 

When instruction A writes the result to register X (including explicit general-purpose registers and implicit special registers), and instruction B needs to use the same register as an input operand, this case is referred to as that instruction B depends on instruction A. If instruction A prepares the result to be written to register X at the end of the SA pipeline stage, and instruction B reads the data in register X at the beginning of the SB pipeline stage, then instruction A must be issued D=max(SA-SB+1, 0\) cycles before instruction B. 

If the processor fetches instruction B less than D cycles after instruction A, the processor delays issuing instruction B until D cycles have passed. The act of a processor delaying an instruction because of pipeline interactions is called an interlock. 

Suppose the SA pipeline stage of instruction A is W and the SB pipeline stage of instruction B is E, instruction B is issued to the pipeline D=max(2-1+1, 0)=2 cycles later than instruction A as shown in Figure 1-6. When the output operand of an instruction is designed to be available at the end of a pipeline stage, it means that the operation of the instruction is over. Usually, instructions that depend on this result data must wait until the output operand is written to the corresponding register before retrieving it from the corresponding register. The Xtensa processor supports the ”bypass” operation. It detects when the input operand of an instruction is generated at which pipeline stage of the instruction and does not need to wait for the data to be written to the register. It can directly forward the data from the pipeline stage where it is generated to the stage where it is needed. 

Espressif Systems 61 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

|  |
| :---- |

Figure 1­6. Interlock Caused by Instruction Operand Dependency 

Data dependencies between instructions are determined by the dependencies between operands and the pipeline stage at which reads and writes happen. Table 1-22 lists all operands of the ESP32-S3 extended instructions, including implicit special register write (def) and read (use) pipeline stage information. 

Table 1­22. Extended Instruction Pipeline Stages 

| Instruction | Operand Pipeline Stage  |  | Special Register Pipeline Stage |  |
| :---- | ----- | ----- | :---- | :---- |
|  | Use  | Def  | Use  | Def |
| EE.ANDQ  | qx 1, qy 1  | qa 1  | —  | — |
| EE.BITREV  | ax 1  | qa 1, ax 1 | FFT\_BIT\_WIDTH 1 | — |
| EE.CLR\_BIT\_GPIO\_OUT  | —  | —  | GPIO\_OUT 1  | GPIO\_OUT 1 |
| EE.CMUL.S16  | qx 1, qy 1  | qz 2  | SAR 1  | — |
| EE.CMUL.S16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qz 2  | SAR 1  | — |
| EE.CMUL.S16.ST.INCP | qv 2, as 1, qx 1, qy 1 | as 1, qz 2  | SAR 1  | — |
| EE.FFT.AMS.S16.LD.INCP | as 1, qx 1, qy 1, qm 1 | qu 2, as 1, qz 2, qz1 2 | SAR 1  | — |
| EE.FFT.AMS.S16.LD.INCP.UAUP | as 1, qx 1, qy 1, qm 1 | qu 2, as 1, qz 2, qz1 2 | SAR 1, SAR\_BYTE  1, UA\_STATE 1 | UA\_STATE 1 |
| EE.FFT.AMS.S16.LD.R32.DECP | as 1, qx 1, qy 1, qm 1 | qu 2, as 1, qz 2, qz1 2 | SAR 1  | — |
| EE.FFT.AMS.S16.ST.INCP | qv 2, as0 1, as 1, qx 1, qy 1, qm 1 | qz1 2, as0 2, as 1 | SAR 1  | — |
| EE.FFT.CMUL.S16.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1, qz 2  | SAR 1  | — |
| EE.FFT.CMUL.S16.ST.XP | qx 1, qy 1, qv 2, as 1, ad 1 | as 1  | SAR 1  | — |

Espressif Systems 62   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.FFT.R2BF.S16  | qx 1, qy 1  | qa0 1, qa1 1  | —  | — |
| :---- | ----- | ----- | :---- | :---- |
| EE.FFT.R2BF.S16.ST.INCP  | qx 1, qy 1, as 1  | qa0 1, as 1  | —  | — |
| EE.FFT.VST.R32.DECP  | qv 2, as 1  | as 1  | —  | — |
| EE.GET\_GPIO\_IN  | —  | au 1  | GPIO\_IN 1  | — |
| EE.LD.128.USAR.IP  | as 1  | qu 2, as 1  | —  | SAR\_BYTE 1 |
| EE.LD.128.USAR.XP  | as 1, ad 1  | qu 2, as 1  | —  | SAR\_BYTE 1 |
| EE.LD.ACCX.IP  | as 1  | as 1  | —  | ACCX 2 |
| EE.LD.QACC\_H.H.32.IP  | as 1  | as 1  | QACC\_H 1  | QACC\_H 2 |
| EE.LD.QACC\_H.L.128.IP  | as 1  | as 1  | QACC\_H 1  | QACC\_H 2 |
| EE.LD.QACC\_L.H.32.IP  | as 1  | as 1  | QACC\_L 1  | QACC\_L 2 |
| EE.LD.QACC\_L.L.128.IP  | as 1  | as 1  | QACC\_L 1  | QACC\_L 2 |
| EE.LD.UA\_STATE.IP  | as 1  | as 1  | —  | UA\_STATE 2 |
| EE.LDF.128.IP  | as 1 | fu3 2, fu2 2, fu1 2, fu0 2, as 1 | —  | — |
| EE.LDF.128.XP  | as 1, ad 1 | fu3 2, fu2 2, fu1 2, fu0 2, as 1 | —  | — |
| EE.LDF.64.IP  | as 1  | fu1 2, fu0 2, as 1  | —  | — |
| EE.LDF.64.XP  | as 1, ad 1  | fu1 2, fu0 2, as 1  | —  | — |
| EE.LDQA.S16.128.IP  | as 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.S16.128.XP  | as 1, ad 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.S8.128.IP  | as 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.S8.128.XP  | as 1, ad 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.U16.128.IP  | as 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.U16.128.XP  | as 1, ad 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.U8.128.IP  | as 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDQA.U8.128.XP  | as 1, ad 1  | as 1  | — | QACC\_L 2, QACC\_H 2 |
| EE.LDXQ.32  | qs 1, as 1  | qu 2  | —  | — |
| EE.MOV.S16.QACC  | qs 1  | —  | — | QACC\_L 1, QACC\_H 1 |
| EE.MOV.S8.QACC  | qs 1  | —  | — | QACC\_L 1, QACC\_H 1 |
| EE.MOV.U16.QACC  | qs 1  | —  | — | QACC\_L 1, QACC\_H 1 |
| EE.MOV.U8.QACC  | qs 1  | —  | — | QACC\_L 1, QACC\_H 1 |
| EE.MOVI.32.A  | qs 1  | au 1  | —  | — |

Espressif Systems 63   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.MOVI.32.Q  | as 1  | qu 1  | —  | — |
| :---- | ----- | ----- | :---- | :---- |
| EE.NOTQ  | qx 1  | qa 1  | —  | — |
| EE.ORQ  | qx 1, qy 1  | qa 1  | —  | — |
| EE.SET\_BIT\_GPIO\_OUT  | —  | —  | GPIO\_OUT 1  | GPIO\_OUT 1 |
| EE.SLCI.2Q  | qs1 1, qs0 1  | qs1 1, qs0 1  | —  | — |
| EE.SLCXXP.2Q | qs1 1, qs0 1, as 1, ad 1 | qs1 1, qs0 1, as 1 | —  | — |
| EE.SRC.Q  | qs0 1, qs1 1  | qa 1  | SAR\_BYTE 1  | — |
| EE.SRC.Q.LD.IP | as 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1  | SAR\_BYTE 1  | — |
| EE.SRC.Q.LD.XP | as 1, ad 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1  | SAR\_BYTE 1  | — |
| EE.SRC.Q.QUP  | qs0 1, qs1 1  | qa 1, qs0 1  | SAR\_BYTE 1  | — |
| EE.SRCI.2Q  | qs1 1, qs0 1  | qs1 1, qs0 1  | —  | — |
| EE.SRCMB.S16.QACC  | as 1  | qu 1 | QACC\_H 1, QACC\_L 1 | QACC\_H 1, QACC\_L 1 |
| EE.SRCMB.S8.QACC  | as 1  | qu 1 | QACC\_H 1, QACC\_L 1 | QACC\_H 1, QACC\_L 1 |
| EE.SRCQ.128.ST.INCP | qs0 1, qs1 1, as 1 | as 1  | SAR\_BYTE 1  | — |
| EE.SRCXXP.2Q | qs1 1, qs0 1, as 1, ad 1 | qs1 1, qs0 1, as 1 | —  | — |
| EE.SRS.ACCX  | as 1  | au 1  | ACCX 1  | ACCX 1 |
| EE.ST.ACCX.IP  | as 1  | as 1  | ACCX 1  | — |
| EE.ST.QACC\_H.H.32.IP  | as 1  | as 1  | QACC\_H 1  | — |
| EE.ST.QACC\_H.L.128.IP  | as 1  | as 1  | QACC\_H 1  | — |
| EE.ST.QACC\_L.H.32.IP  | as 1  | as 1  | QACC\_L 1  | — |
| EE.ST.QACC\_L.L.128.IP  | as 1  | as 1  | QACC\_L 1  | — |
| EE.ST.UA\_STATE.IP  | as 1  | as 1  | UA\_STATE 1  | — |
| EE.STF.128.IP | fv3 1, fv2 1, fv1 1, fv0 1, as 1 | as 1  | —  | — |
| EE.STF.128.XP | fv3 1, fv2 1, fv1 1, fv0 1, as 1, ad 1 | as 1  | —  | — |
| EE.STF.64.IP  | fv1 1, fv0 1, as 1  | as 1  | —  | — |
| EE.STF.64.XP | fv1 1, fv0 1, as 1, ad 1 | as 1  | —  | — |
| EE.STXQ.32  | qv 1, qs 1, as 1  | —  | —  | — |
| EE.VADDS.S16  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VADDS.S16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VADDS.S16.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VADDS.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VADDS.S32.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |

Espressif Systems 64   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.VADDS.S32.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| :---- | ----- | ----- | :---- | :---- |
| EE.VADDS.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VADDS.S8.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VADDS.S8.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VCMP.EQ.S16  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.EQ.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.EQ.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.GT.S16  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.GT.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.GT.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.LT.S16  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.LT.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VCMP.LT.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VLD.128.IP  | as 1  | qu 2, as 1  | —  | — |
| EE.VLD.128.XP  | as 1, ad 1  | qu 2, as 1  | —  | — |
| EE.VLD.H.64.IP  | as 1  | qu 2, as 1  | —  | — |
| EE.VLD.H.64.XP  | as 1, ad 1  | qu 2, as 1  | —  | — |
| EE.VLD.L.64.IP  | as 1  | qu 2, as 1  | —  | — |
| EE.VLD.L.64.XP  | as 1, ad 1  | qu 2, as 1  | —  | — |
| EE.VLDBC.16  | as 1  | qu 2  | —  | — |
| EE.VLDBC.16.IP  | as 1  | qu 2, as 1  | —  | — |
| EE.VLDBC.16.XP  | as 1, ad 1  | qu 2, as 1  | —  | — |
| EE.VLDBC.32  | as 1  | qu 2  | —  | — |
| EE.VLDBC.32.IP  | as 1  | qu 2, as 1  | —  | — |
| EE.VLDBC.32.XP  | as 1, ad 1  | qu 2, as 1  | —  | — |
| EE.VLDBC.8  | as 1  | qu 2  | —  | — |
| EE.VLDBC.8.IP  | as 1  | qu 2, as 1  | —  | — |
| EE.VLDBC.8.XP  | as 1, ad 1  | qu 2, as 1  | —  | — |
| EE.VLDHBC.16.INCP  | as 1  | qu 2, qu1 2, as 1  | —  | — |
| EE.VMAX.S16  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VMAX.S16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VMAX.S16.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VMAX.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VMAX.S32.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VMAX.S32.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VMAX.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VMAX.S8.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VMAX.S8.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VMIN.S16  | qx 1, qy 1  | qa 1  | —  | — |

Espressif Systems 65   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.VMIN.S16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| :---- | ----- | ----- | :---- | :---- |
| EE.VMIN.S16.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VMIN.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VMIN.S32.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VMIN.S32.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VMIN.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VMIN.S8.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VMIN.S8.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VMUL.S16  | qx 1, qy 1  | qz 2  | SAR 1  | — |
| EE.VMUL.S16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qz 2  | SAR 1  | — |
| EE.VMUL.S16.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qz 2  | SAR 1  | — |
| EE.VMUL.S8  | qx 1, qy 1  | qz 2  | SAR 1  | — |
| EE.VMUL.S8.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qz 2  | SAR 1  | — |
| EE.VMUL.S8.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qz 2  | SAR 1  | — |
| EE.VMUL.U16  | qx 1, qy 1  | qz 2  | SAR 1  | — |
| EE.VMUL.U16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qz 2  | SAR 1  | — |
| EE.VMUL.U16.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qz 2  | SAR 1  | — |
| EE.VMUL.U8  | qx 1, qy 1  | qz 2  | SAR 1  | — |
| EE.VMUL.U8.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qz 2  | SAR 1  | — |
| EE.VMUL.U8.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qz 2  | SAR 1  | — |
| EE.VMULAS.S16.ACCX  | qx 1, qy 1  | —  | ACCX 2  | ACCX 2 |
| EE.VMULAS.S16.ACCX.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.S16.ACCX.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.S16.ACCX.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.S16.ACCX.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.S16.QACC  | qx 1, qy 1  | — | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S16.QACC.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S16.QACC.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |

Espressif Systems 66   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.VMULAS.S16.QACC.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| ----- | ----- | ----- | :---- | :---- |
| EE.VMULAS.S16.QACC.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S16.QACC.LDBC.INCP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S16.QACC.LDBC.INCP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S8.ACCX  | qx 1, qy 1  | —  | ACCX 2  | ACCX 2 |
| EE.VMULAS.S8.ACCX.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.S8.ACCX.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.S8.ACCX.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.S8.ACCX.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.S8.QACC  | qx 1, qy 1  | — | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.S8.QACC.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S8.QACC.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S8.QACC.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S8.QACC.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S8.QACC.LDBC.INCP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.S8.QACC.LDBC.INCP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.U16.ACCX  | qx 1, qy 1  | —  | ACCX 2  | ACCX 2 |
| EE.VMULAS.U16.ACCX.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.U16.ACCX.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.U16.ACCX.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1  | ACCX 2  | ACCX 2 |

Espressif Systems 67   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.VMULAS.U16.ACCX.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| ----- | ----- | ----- | :---- | :---- |
| EE.VMULAS.U16.QACC  | qx 1, qy 1  | — | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U16.QACC.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U16.QACC.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.U16.QACC.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1 | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U16.QACC.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.U16.QACC.LDBC.INCP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U16.QACC.LDBC.INCP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.U8.ACCX  | qx 1, qy 1  | —  | ACCX 2  | ACCX 2 |
| EE.VMULAS.U8.ACCX.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.U8.ACCX.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.U8.ACCX.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1  | ACCX 2  | ACCX 2 |
| EE.VMULAS.U8.ACCX.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, ACCX 2 | ACCX 2 |
| EE.VMULAS.U8.QACC  | qx 1, qy 1  | — | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U8.QACC.LD.IP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U8.QACC.LD.IP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.U8.QACC.LD.XP | as 1, ad 1, qx 1, qy 1 | qu 2, as 1 | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VMULAS.U8.QACC.LD.XP.QUP | as 1, ad 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VMULAS.U8.QACC.LDBC.INCP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |

Espressif Systems 68   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.VMULAS.U8.QACC.LDBC.INCP.QUP | as 1, qx 1, qy 1, qs0 1, qs1 1 | qu 2, as 1, qs0 1 | SAR\_BYTE 1, QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| :---- | ----- | ----- | :---- | :---- |
| EE.VPRELU.S16  | qx 1, qy 1, ay 1  | qz 2  | —  | — |
| EE.VPRELU.S8  | qx 1, qy 1, ay 1  | qz 2  | —  | — |
| EE.VRELU.S16  | qs 1, ax 1, ay 1  | qs 2  | —  | — |
| EE.VRELU.S8  | qs 1, ax 1, ay 1  | qs 2  | —  | — |
| EE.VSL.32  | qs 1  | qa 1  | SAR 1  | — |
| EE.VSMULAS.S16.QACC  | qx 1, qy 1  | — | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VSMULAS.S16.QACC.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VSMULAS.S8.QACC  | qx 1, qy 1  | — | QACC\_L 2, QACC\_H 2 | QACC\_L 2, QACC\_H 2 |
| EE.VSMULAS.S8.QACC.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1 | QACC\_H 2, QACC\_L 2 | QACC\_H 2, QACC\_L 2 |
| EE.VSR.32  | qs 1  | qa 1  | SAR 1  | — |
| EE.VST.128.IP  | qv 1, as 1  | as 1  | —  | — |
| EE.VST.128.XP  | qv 1, as 1, ad 1  | as 1  | —  | — |
| EE.VST.H.64.IP  | qv 1, as 1  | as 1  | —  | — |
| EE.VST.H.64.XP  | qv 1, as 1, ad 1  | as 1  | —  | — |
| EE.VST.L.64.IP  | qv 1, as 1  | as 1  | —  | — |
| EE.VST.L.64.XP  | qv 1, as 1, ad 1  | as 1  | —  | — |
| EE.VSUBS.S16  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VSUBS.S16.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VSUBS.S16.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VSUBS.S32  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VSUBS.S32.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VSUBS.S32.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VSUBS.S8  | qx 1, qy 1  | qa 1  | —  | — |
| EE.VSUBS.S8.LD.INCP  | as 1, qx 1, qy 1  | qu 2, as 1, qa 1  | —  | — |
| EE.VSUBS.S8.ST.INCP | qv 1, as 1, qx 1, qy 1 | as 1, qa 1  | —  | — |
| EE.VUNZIP.16  | qs0 1, qs1 1  | qs0 1, qs1 1  | —  | — |
| EE.VUNZIP.32  | qs0 1, qs1 1  | qs0 1, qs1 1  | —  | — |
| EE.VUNZIP.8  | qs0 1, qs1 1  | qs0 1, qs1 1  | —  | — |
| EE.VZIP.16  | qs0 1, qs1 1  | qs0 1, qs1 1  | —  | — |
| EE.VZIP.32  | qs0 1, qs1 1  | qs0 1, qs1 1  | —  | — |
| EE.VZIP.8  | qs0 1, qs1 1  | qs0 1, qs1 1  | —  | — |
| EE.WR\_MASK\_GPIO\_OUT  | as 1, ax 1  | —  | GPIO\_OUT 1  | GPIO\_OUT 1 |
| EE.XORQ  | qx 1, qy 1  | qa 1  | —  | — |
| EE.ZERO.ACCX  | —  | —  | —  | ACCX 1 |

Espressif Systems 69   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

| EE.ZERO.Q  | —  | qa 1  | —  | — |
| :---- | :---- | :---- | :---- | :---- |
| EE.ZERO.QACC  | —  | —  | — | QACC\_L 1, QACC\_H 1 |

1.7.2 Hardware Resource Hazard 

When multiple instructions call the same hardware resource at the same time, the processor allows only one of the instructions to occupy the hardware resource, and the rest of them will be delayed. For example, there are only eight 16-bit multipliers in the processor; instruction C requires eight of them in pipeline stage M, and instruction D requires four of them in pipeline stage E. As shown in Figure 1-7, instruction C is issued in cycle T+0, and instruction D is issued in cycle T+1, so four multipliers are applied to be occupied simultaneously in cycle T+3; at this time, the processor will delay the issue of instruction D into the pipeline by one cycle to avoid conflict with instruction C. 

|  |
| :---- |

Figure 1­7. Hardware Resource Hazard 

1.7.3 Control Hazard 

Data and hardware resource hazards can be optimized by adjusting the code order, but the control hazard is difficult to optimize. Program code usually has many conditional select statements that execute different code depending on whether the condition is met or not. The compiler will process the above conditional statements into branch and jump instructions: if the condition is satisfied, it will jump to the target address to execute the corresponding code; if not, the subsequent instructions will be processed in order. When the conditions are met, as shown in Figure 1-8, the processor will re-fetch the instruction from the new target address. At this time, the instructions at the R and E stages on the pipeline will be removed, which means the pipeline remains stagnant for 2 cycles. 

Espressif Systems 70 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

|  |
| :---- |

Figure 1­8. Control Hazard 

Espressif Systems 71   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

1.8 Extended Instruction Functional Description 

Before reading this section, you are recommended to read the table 1-1, which introduces instruction field names and their meanings in instruction encoding. 

\[N:M\] is used to represent the field. It means the width of the field is (N-M+1), namely, both bits N and M are included. For example, qa\[2:0\] has a total of 3 bits, which are bit0, bit1 and bit2, and qa\[1\] represents the value of bit1. 

This chapter describes all the instructions mentioned in Section 1.6 in alphabetical order by the instruction name. Each instruction is encoded in little-endian bit order as shown in Figure 1-2. 

1.8.1 EE.ANDQ 

Instruction Word 

| 11  | qa\[2:1\]  | 1101  | qa\[0\]  | 011  | qy\[2:1\]  | 00  | qx\[2:1\]  | qy\[0\]  | qx\[0\]  | 0100 |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.ANDQ qa, qx, qy 

Description 

This instruction performs a bitwise AND operation on registers qx and qy and writes the result of the logical operation to register qa. 

Operation 

1 qa \= qx & qy 

Espressif Systems 72 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.8.2 EE.BITREV 

Instruction Word 

| 11  | qa\[2:1\]  | 1101  | qa\[0\]  | 1111011  | as\[3:0\]  | 0100 |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.BITREV qa, as 

Description 

This instruction swaps the bit order of data of different bit widths according to the value of special register FFT\_BIT\_WIDTH. Then, it compares the data before and after the swap, takes the larger value, pads the higher bits with 0 until it has 16 bits, and writes the result into the corresponding data segment of register qa. In the following, Switchx is a function that represents the bit inversion of the x-bit. Switch5(0b10100) \= 0b00101. Here 0b10100 means 5-bit binary data. 

Operation 

1 temp0\[15:0\] \= as\[15:0\] 

2 temp1\[15:0\] \= as\[15:0\] \+ 1 

3 temp2\[15:0\] \= as\[15:0\] \+ 2 

4 temp3\[15:0\] \= as\[15:0\] \+ 3 

5 temp4\[15:0\] \= as\[15:0\] \+ 4 

6 temp5\[15:0\] \= as\[15:0\] \+ 5 

7 temp6\[15:0\] \= as\[15:0\] \+ 6 

8 temp7\[15:0\] \= as\[15:0\] \+ 7 

9 if FFT\_BIT\_WIDTH==3: 

10 Switch3(X\[2:0\]) \= X\[0:2\] 

11 qa\[ 15: 0\] \= {13’h0, max(tmp0\[2:0\], Switch3(tmp0\[2:0\]))} 

12 qa\[ 31: 16\] \= {13’h0, max(tmp1\[2:0\], Switch3(tmp1\[2:0\]))} 

13 qa\[ 47: 32\] \= {13’h0, max(tmp2\[2:0\], Switch3(tmp2\[2:0\]))} 

14 qa\[ 63: 48\] \= {13’h0, max(tmp3\[2:0\], Switch3(tmp3\[2:0\]))} 

15 qa\[ 79: 64\] \= {13’h0, max(tmp4\[2:0\], Switch3(tmp4\[2:0\]))} 

16 qa\[ 95: 80\] \= {13’h0, max(tmp5\[2:0\], Switch3(tmp5\[2:0\]))} 

17 qa\[127: 96\] \= 0 

18 if FFT\_BIT\_WIDTH==4: 

19 Switch4(X\[3:0\]) \= X\[0:3\] 

20 qa\[ 15: 0\] \= {12’h0, max(tmp0\[3:0\], Switch4(tmp0\[3:0\]))} 

21 qa\[ 31: 16\] \= {12’h0, max(tmp1\[3:0\], Switch4(tmp1\[3:0\]))} 

22 qa\[ 47: 32\] \= {12’h0, max(tmp2\[3:0\], Switch4(tmp2\[3:0\]))} 

23 qa\[ 63: 48\] \= {12’h0, max(tmp3\[3:0\], Switch4(tmp3\[3:0\]))} 

24 qa\[ 79: 64\] \= {12’h0, max(tmp4\[3:0\], Switch4(tmp4\[3:0\]))} 

25 qa\[ 95: 80\] \= {12’h0, max(tmp5\[3:0\], Switch4(tmp5\[3:0\]))} 

26 qa\[111: 96\] \= {12’h0, max(tmp6\[3:0\], Switch4(tmp6\[3:0\]))} 

27 qa\[127:112\] \= {12’h0, max(tmp7\[3:0\], Switch4(tmp7\[3:0\]))} 

28 ... 

29 if FFT\_BIT\_WIDTH==10: 

30 Switch10(X\[9:0\]) \= X\[0:9\] 

31 qa\[ 15: 0\] \= {6’h0, max(tmp0\[9:0\], Switch10(tmp0\[9:0\]))} 

32 qa\[ 31: 16\] \= {6’h0, max(tmp1\[9:0\], Switch10(tmp1\[9:0\]))} 

33 qa\[ 47: 32\] \= {6’h0, max(tmp2\[9:0\], Switch10(tmp2\[9:0\]))} 

34 qa\[ 63: 48\] \= {6’h0, max(tmp3\[9:0\], Switch10(tmp3\[9:0\]))} 

35 qa\[ 79: 64\] \= {6’h0, max(tmp4\[9:0\], Switch10(tmp4\[9:0\]))} 

36 qa\[ 95: 80\] \= {6’h0, max(tmp5\[9:0\], Switch10(tmp5\[9:0\]))} 

Espressif Systems 73 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

37 qa\[111: 96\] \= {6’h0, max(tmp6\[9:0\], Switch10(tmp6\[9:0\]))} 

38 qa\[127:112\] \= {6’h0, max(tmp7\[9:0\], Switch10(tmp7\[9:0\]))} 

39 

40 as\[31:0\] \= as\[31:0\] \+ 8 

Espressif Systems 74   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

1.8.3 EE.CLR\_BIT\_GPIO\_OUT 

Instruction Word 

| 011101100100  | imm256\[7:0\]  | 0100 |
| :---: | :---: | :---: |

Assembler Syntax 

EE.CLR\_BIT\_GPIO\_OUT 0..255 

Description 

It is a dedicated CPU GPIO instruction to clear certain GPIO\_OUT bits. The content to clear depends on the 8-bit immediate number imm256. 

Operation 

1 GPIO\_OUT\[7:0\] \= (GPIO\_OUT\[7:0\] & \~imm256\[7:0\]) 

Espressif Systems 75   
ESP32-S3 TRM (Version 1.2)

Submit Documentation Feedback   
1 Processor Instruction Extensions (PIE) GoBack 

1.8.4 EE.CMUL.S16 

Instruction Word 

| 10  | qz\[2:1\]  | 1110  | qz\[0\]  | qy\[2\]  | 0  | qy\[1:0\]  | qx\[2:0\]  | 00  | sel4\[1:0\]  | 0100 |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.CMUL.S16 qz, qx, qy, 0..3 

Description 

This instruction performs a 16-bit signed complex multiplication. The range of the immediate number sel4 is 0 \~ 3, which specifies the 32 bits in the two QR registers qx and qy for complex multiplication. The real and imaginary parts of complex numbers are stored in the upper 16 bits and lower 16 bits of the 32 bits respectively. The calculated real part and imaginary part results are stored in the corresponding 32 bits of register qz. Operation 

1 if sel4 \== 0: 

2 qz\[ 15: 0\] \= (qx\[ 15: 0\] \* qy\[ 15: 0\] \- qx\[ 31: 16\] \* qy\[ 31: 16\]) \>\> SAR\[5:0\] 3 qz\[ 31: 16\] \= (qx\[ 15: 0\] \* qy\[ 31: 16\] \+ qx\[ 31: 16\] \* qy\[ 15: 0\]) \>\> SAR\[5:0\] 4 qz\[ 47: 32\] \= (qx\[ 47: 32\] \* qy\[ 47: 32\] \- qx\[ 63: 48\] \* qy\[ 63: 48\]) \>\> SAR\[5:0\] 5 qz\[ 63: 48\] \= (qx\[ 47: 32\] \* qy\[ 63: 48\] \+ qx\[ 63: 48\] \* qy\[ 47: 32\]) \>\> SAR\[5:0\] 6 if sel4 \== 1: 

7 qz\[ 79: 64\] \= (qx\[ 79: 64\] \* qy\[ 79: 64\] \- qx\[ 95: 80\] \* qy\[ 95: 80\]) \>\> SAR\[5:0\] 8 qz\[ 95: 80\] \= (qx\[ 79: 64\] \* qy\[ 95: 80\] \+ qx\[ 95: 80\] \* qy\[ 79: 64\]) \>\> SAR\[5:0\] 9 qz\[111: 96\] \= (qx\[111: 96\] \* qy\[111: 96\] \- qx\[127:112\] \* qy\[127:112\]) \>\> SAR\[5:0\] 

10 qz\[127:112\] \= (qx\[111: 96\] \* qy\[127:112\] \+ qx\[127:112\] \* qy\[111: 96\]) \>\> SAR\[5:0\] 11 if sel4 \== 2: 

12 qz\[ 15: 0\] \= (qx\[ 15: 0\] \* qy\[ 15: 0\] \+ qx\[ 31: 16\] \* qy\[ 31: 16\]) \>\> SAR\[5:0\] 13 qz\[ 31: 16\] \= (qx\[ 15: 0\] \* qy\[ 31: 16\] \- qx\[ 31: 16\] \* qy\[ 15: 0\]) \>\> SAR\[5:0\] 14 qz\[ 47: 32\] \= (qx\[ 47: 32\] \* qy\[ 47: 32\] \+ qx\[ 63: 48\] \* qy\[ 63: 48\]) \>\> SAR\[5:0\] 15 qz\[ 63: 48\] \= (qx\[ 47: 32\] \* qy\[ 63: 48\] \- qx\[ 63: 48\] \* qy\[ 47: 32\]) \>\> SAR\[5:0\] 16 if sel4 \== 3: 

17 qz\[ 79: 64\] \= (qx\[ 79: 64\] \* qy\[ 79: 64\] \+ qx\[ 95: 80\] \* qy\[ 95: 80\]) \>\> SAR\[5:0\] 18 qz\[ 95: 80\] \= (qx\[ 79: 64\] \* qy\[ 95: 80\] \- qx\[ 95: 80\] \* qy\[ 79: 64\]) \>\> SAR\[5:0\] 19 qz\[111: 96\] \= (qx\[111: 96\] \* qy\[111: 96\] \+ qx\[127:112\] \* qy\[127:112\]) \>\> SAR\[5:0\] 20 qz\[127:112\] \= (qx\[111: 96\] \* qy\[127:112\] \- qx\[127:112\] \* qy\[111: 96\]) \>\> SAR\[5:0\] 

Espressif Systems 76 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.8.5 EE.CMUL.S16.LD.INCP 

Instruction Word 

| 111000  | qu\[2:1\]  | qy\[0\]  | 000  | qu\[0\]  | qz\[2:0\]  | qx\[1:0\]  | qy\[2:1\]  | 11  | sel4\[1:0\]  | as\[3:0\]  | 111  | qx\[2\] |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.CMUL.S16.LD.INCP qu, as, qz, qx, qy, 0..3 

Description 

This instruction performs a 16-bit signed complex multiplication. The range of the immediate number sel4 is 0 \~ 7, which specifies the 32 bits in the two QR registers qx and qy for complex multiplication. The real and imaginary parts of complex numbers are stored in the upper 16 bits and lower 16 bits of the 32 bits respectively. The calculated real part and imaginary part results are stored in the corresponding 32 bits of register qz. During the operation, the lower 4 bits of the access address in register as are forced to be 0, and then the 16-byte data is loaded from the memory to register qu. After the access, the value in register as is incremented by 16\. 

Operation 

1 if sel4 \== 0: 

2 qz\[ 15: 0\] \= (qx\[ 15: 0\] \* qy\[ 15: 0\] \- qx\[ 31: 16\] \* qy\[ 31: 16\]) \>\> SAR\[5:0\] 3 qz\[ 31: 16\] \= (qx\[ 15: 0\] \* qy\[ 31: 16\] \+ qx\[ 31: 16\] \* qy\[ 15: 0\]) \>\> SAR\[5:0\] 4 qz\[ 47: 32\] \= (qx\[ 47: 32\] \* qy\[ 47: 32\] \- qx\[ 63: 48\] \* qy\[ 63: 48\]) \>\> SAR\[5:0\] 5 qz\[ 63: 48\] \= (qx\[ 47: 32\] \* qy\[ 63: 48\] \+ qx\[ 63: 48\] \* qy\[ 47: 32\]) \>\> SAR\[5:0\] 6 if sel4 \== 1: 

7 qz\[ 79: 64\] \= (qx\[ 79: 64\] \* qy\[ 79: 64\] \- qx\[ 95: 80\] \* qy\[ 95: 80\]) \>\> SAR\[5:0\] 8 qz\[ 95: 80\] \= (qx\[ 79: 64\] \* qy\[ 95: 80\] \+ qx\[ 95: 80\] \* qy\[ 79: 64\]) \>\> SAR\[5:0\] 9 qz\[111: 96\] \= (qx\[111: 96\] \* qy\[111: 96\] \- qx\[127:112\] \* qy\[127:112\]) \>\> SAR\[5:0\] 

10 qz\[127:112\] \= (qx\[111: 96\] \* qy\[127:112\] \+ qx\[127:112\] \* qy\[111: 96\]) \>\> SAR\[5:0\] 11 if sel4 \== 2: 

12 qz\[ 15: 0\] \= (qx\[ 15: 0\] \* qy\[ 15: 0\] \+ qx\[ 31: 16\] \* qy\[ 31: 16\]) \>\> SAR\[5:0\] 13 qz\[ 31: 16\] \= (qx\[ 15: 0\] \* qy\[ 31: 16\] \- qx\[ 31: 16\] \* qy\[ 15: 0\]) \>\> SAR\[5:0\] 14 qz\[ 47: 32\] \= (qx\[ 47: 32\] \* qy\[ 47: 32\] \+ qx\[ 63: 48\] \* qy\[ 63: 48\]) \>\> SAR\[5:0\] 15 qz\[ 63: 48\] \= (qx\[ 47: 32\] \* qy\[ 63: 48\] \- qx\[ 63: 48\] \* qy\[ 47: 32\]) \>\> SAR\[5:0\] 16 if sel4 \== 3: 

17 qz\[ 79: 64\] \= (qx\[ 79: 64\] \* qy\[ 79: 64\] \+ qx\[ 95: 80\] \* qy\[ 95: 80\]) \>\> SAR\[5:0\] 18 qz\[ 95: 80\] \= (qx\[ 79: 64\] \* qy\[ 95: 80\] \- qx\[ 95: 80\] \* qy\[ 79: 64\]) \>\> SAR\[5:0\] 19 qz\[111: 96\] \= (qx\[111: 96\] \* qy\[111: 96\] \+ qx\[127:112\] \* qy\[127:112\]) \>\> SAR\[5:0\] 20 qz\[127:112\] \= (qx\[111: 96\] \* qy\[127:112\] \- qx\[127:112\] \* qy\[111: 96\]) \>\> SAR\[5:0\] 21 

22 qu\[127:0\] \= load128({as\[31:4\],4{0}}) 

23 as\[31:0\] \= as\[31:0\] \+ 16 

Espressif Systems 77 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.8.6 EE.CMUL.S16.ST.INCP 

Instruction Word 

| 11100100  | qy\[0\]  | qv\[2:0\]  | 0  | qz\[2:0\]  | qx\[1:0\]  | qy\[2:1\]  | 00  | sel4\[1:0\]  | as\[3:0\]  | 111  | qx\[2\] |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.CMUL.S16.ST.INCP qv, as, qz, qx, qy, sel4 

Description 

This instruction performs a 16-bit signed complex multiplication. The range of the immediate number sel4 is 0 \~ 7, which specifies the 32 bits in the two QR registers qx and qy for complex multiplication. The real and imaginary parts of complex numbers are stored in the upper 16 bits and lower 16 bits of the 32 bits respectively. The calculated real part and imaginary part results are stored in the corresponding 32 bits of register qz. During the operation, the lower 4 bits of the access address in register as are forced to be 0, and then stores the 16-byte data of qv to memory. After the access, the value in register as is incremented by 16\. Operation 

1 if sel4 \== 0: 

2 qz\[ 15: 0\] \= (qx\[ 15: 0\] \* qy\[ 15: 0\] \- qx\[ 31: 16\] \* qy\[ 31: 16\]) \>\> SAR\[5:0\] 3 qz\[ 31: 16\] \= (qx\[ 15: 0\] \* qy\[ 31: 16\] \+ qx\[ 31: 16\] \* qy\[ 15: 0\]) \>\> SAR\[5:0\] 4 qz\[ 47: 32\] \= (qx\[ 47: 32\] \* qy\[ 47: 32\] \- qx\[ 63: 48\] \* qy\[ 63: 48\]) \>\> SAR\[5:0\] 5 qz\[ 63: 48\] \= (qx\[ 47: 32\] \* qy\[ 63: 48\] \+ qx\[ 63: 48\] \* qy\[ 47: 32\]) \>\> SAR\[5:0\] 6 if sel4 \== 1: 

7 qz\[ 79: 64\] \= (qx\[ 79: 64\] \* qy\[ 79: 64\] \- qx\[ 95: 80\] \* qy\[ 95: 80\]) \>\> SAR\[5:0\] 8 qz\[ 95: 80\] \= (qx\[ 79: 64\] \* qy\[ 95: 80\] \+ qx\[ 95: 80\] \* qy\[ 79: 64\]) \>\> SAR\[5:0\] 9 qz\[111: 96\] \= (qx\[111: 96\] \* qy\[111: 96\] \- qx\[127:112\] \* qy\[127:112\]) \>\> SAR\[5:0\] 

10 qz\[127:112\] \= (qx\[111: 96\] \* qy\[127:112\] \+ qx\[127:112\] \* qy\[111: 96\]) \>\> SAR\[5:0\] 11 if sel4 \== 2: 

12 qz\[ 15: 0\] \= (qx\[ 15: 0\] \* qy\[ 15: 0\] \+ qx\[ 31: 16\] \* qy\[ 31: 16\]) \>\> SAR\[5:0\] 13 qz\[ 31: 16\] \= (qx\[ 15: 0\] \* qy\[ 31: 16\] \- qx\[ 31: 16\] \* qy\[ 15: 0\]) \>\> SAR\[5:0\] 14 qz\[ 47: 32\] \= (qx\[ 47: 32\] \* qy\[ 47: 32\] \+ qx\[ 63: 48\] \* qy\[ 63: 48\]) \>\> SAR\[5:0\] 15 qz\[ 63: 48\] \= (qx\[ 47: 32\] \* qy\[ 63: 48\] \- qx\[ 63: 48\] \* qy\[ 47: 32\]) \>\> SAR\[5:0\] 16 if sel4 \== 3: 

17 qz\[ 79: 64\] \= (qx\[ 79: 64\] \* qy\[ 79: 64\] \+ qx\[ 95: 80\] \* qy\[ 95: 80\]) \>\> SAR\[5:0\] 18 qz\[ 95: 80\] \= (qx\[ 79: 64\] \* qy\[ 95: 80\] \- qx\[ 95: 80\] \* qy\[ 79: 64\]) \>\> SAR\[5:0\] 19 qz\[111: 96\] \= (qx\[111: 96\] \* qy\[111: 96\] \+ qx\[127:112\] \* qy\[127:112\]) \>\> SAR\[5:0\] 20 qz\[127:112\] \= (qx\[111: 96\] \* qy\[127:112\] \- qx\[127:112\] \* qy\[111: 96\]) \>\> SAR\[5:0\] 21 

22 qv\[127:0\] \=\> store128({as\[31:4\],4{0}}) 

23 as\[31:0\] \= as\[31:0\] \+ 16 

Espressif Systems 78 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.8.7 EE.FFT.AMS.S16.LD.INCP 

Instruction Word 

| 110100  | sel2\[0\]  | qz1\[2\]  | qz\[0\]  | qy\[2:0\]  | qz1\[1\]  | qm\[2:0\]  | qx\[1:0\]  | qz\[2:1\]  | qz1\[0\]  | qu\[2:0\]  | as\[3:0\]  | 111  | qx\[2\] |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.FFT.AMS.S16.LD.INCP qu, as, qz, qz1, qx, qy, qm, sel2 

Description 

It is a dedicated FFT instruction to perform addition, subtraction, multiplication, addition and subtraction, and shift operations on 16-bit data segments. 

During the operation, the lower 4 bits of the access address in register as are forced to be 0, and then the 16-byte data is loaded from the memory to register qu. After the access, the value in register as is incremented by 16\. 

Operation 

1 temp0\[15:0\] \= qx\[ 47: 32\] \+ qy\[ 47: 32\] 

2 temp1\[15:0\] \= qx\[ 63: 48\] \- qy\[ 63: 48\] 

3 

4 if sel2==0: 

5 temp2\[15:0\] \= ((qx\[ 47: 32\] \- qy\[ 47: 32\]) \* qm\[ 47: 32\] \- (qx\[ 63: 48\] \+ qy\[ 63: 48\]) \* qm\[ 63: 48\]) \>\> SAR 

6 temp3\[15:0\] \= ((qx\[ 47: 32\] \- qy\[ 47: 32\]) \* qm\[ 63: 48\] \+ (qx\[ 63: 48\] \+ qy\[ 63: 48\]) \* qm\[ 47: 32\]) \>\> SAR 

7 if sel2==1: 

8 temp2\[15:0\] \= ((qx\[ 63: 48\] \+ qy\[ 63: 48\]) \* qm\[ 63: 48\] \+ (qx\[ 47: 32\] \- qy\[ 47: 32\]) \* qm\[ 47: 32\]) \>\> SAR 

9 temp3\[15:0\] \= ((qx\[ 63: 48\] \+ qy\[ 63: 48\]) \* qm\[ 47: 32\] \- (qx\[ 47: 32\] \- qy\[ 47: 32\]) \* qm\[ 63: 48\]) \>\> SAR 

10 

11 qz\[47: 32\] \= temp0\[15:0\] \+ temp2\[15:0\] 

12 qz\[63: 48\] \= temp1\[15:0\] \+ temp3\[15:0\] 

13 qz1\[47: 32\] \= temp0\[15:0\] \- temp2\[15:0\] 

14 qz1\[63: 48\] \= temp3\[15:0\] \- temp1\[15:0\] 

15 qu \= load128({as\[31:4\],4{0}}) 

16 as\[31:0\] \= as\[31:0\] \+ 16 

Espressif Systems 79 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)  
1 Processor Instruction Extensions (PIE) GoBack 

1.8.8 EE.FFT.AMS.S16.LD.INCP.UAUP 

Instruction Word 

| 110101  | sel2\[0\]  | qz1\[2\]  | qz\[0\]  | qy\[2:0\]  | qz1\[1\]  | qm\[2:0\]  | qx\[1:0\]  | qz\[2:1\]  | qz1\[0\]  | qu\[2:0\]  | as\[3:0\]  | 111  | qx\[2\] |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |

Assembler Syntax 

EE.FFT.AMS.S16.LD.INCP.UAUP qu, as, qz, qz1, qx, qy, qm, sel2 

Description 

It is a dedicated FFT instruction to perform addition, subtraction, multiplication, addition and subtraction, and shift operations on 16-bit data segments. 

During the operation, the lower 4 bits of the access address in the register as are forced to be 0, and then the 16-byte data is loaded from the memory. The instruction joins the loaded data and the data in special register UA\_STATE into 32-byte data, right-shifts the 32-byte data by the result of the SAR\_BYTE value multiplied by 8, and assigns the lower 128 bits of the shifted result to register qu. Meanwhile, register UA\_STATE is updated with the loaded 16-byte data. After the access, the value in register as is incremented by 16\. 

Operation 

1 temp0\[15:0\] \= qx\[ 15: 0\] \+ qy\[ 15: 0\] 

2 temp1\[15:0\] \= qx\[ 31: 16\] \- qy\[ 31: 16\] 

3 

4 if sel2==0: 

5 temp2\[15:0\] \= ((qx\[ 15: 0\] \- qy\[ 15: 0\]) \* qm\[ 15: 0\] \- (qx\[ 31: 16\] \+ qy\[ 31: 16\]) \* qm\[ 31: 16\]) \>\> SAR 

6 temp3\[15:0\] \= ((qx\[ 15: 0\] \- qy\[ 15: 0\]) \* qm\[ 31: 16\] \+ (qx\[ 31: 16\] \+ qy\[ 31: 16\]) \* qm\[ 15: 0\]) \>\> SAR 

7 if sel2==1: 

8 temp2\[15:0\] \= ((qx\[ 31: 16\] \+ qy\[ 31: 16\]) \* qm\[ 31: 16\] \+ (qx\[ 15: 0\] \- qy\[ 15: 0\]) \* qm\[ 15: 0\]) \>\> SAR 

9 temp3\[15:0\] \= ((qx\[ 31: 16\] \+ qy\[ 31: 16\]) \* qm\[ 15: 0\] \- (qx\[ 15: 0\] \- qy\[ 15: 0\]) \* qm\[ 31: 16\]) \>\> SAR 

10 

11 dataIn\[127:0\] \= load128({as\[31:4\],4{0}}) 

12 qz\[15: 0\] \= temp0\[15:0\] \+ temp2\[15:0\] 

13 qz\[31: 16\] \= temp1\[15:0\] \+ temp3\[15:0\] 

14 qz1\[15: 0\] \= temp0\[15:0\] \- temp2\[15:0\] 

15 qz1\[31:16\] \= temp3\[15:0\] \- temp1\[15:0\] 

16 qu\[127: 0\] \= {dataIn\[127:0\], UA\_STATE\[127:0\]} \>\> {SAR\_BYTE\[3:0\] \<\< 3} 

17 UA\_STATE\[127:0\] \= dataIn\[127:0\] 

18 as\[31:0\] \= as\[31:0\] \+ 16 

Espressif Systems 80 Submit Documentation Feedback   
ESP32-S3 TRM (Version 1.2)