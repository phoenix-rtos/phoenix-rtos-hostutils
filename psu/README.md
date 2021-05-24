# psu

SDP script syntax:

- WAIT `<vid>` `<pid>`

  Description: Wait on HID device.

- WRITE\_FILE `<F/S>` `<quoted string>` `[offset]` `[address]` `[size]`
  
  Description: Host sends file or data to HID device.
  
  Example:

  ```
  WRITE_FILE F "phoenix-arm-imx6ull.img" 0 0xac000000 16384   # off=0 addr=0xac000000 size=16384
  ```

  ```
  WRITE_FILE F "phoenix-arm-imx6ull.img" 16384 0xac000000     # off=16384 addr=0xac000000 size=<whole file>
  ```

  ```
  WRITE_FILE S "\x00\x00\x00\x80\xff\xff\xff\x87\x00\x00\x00\x00\x00\x80\x3a\x49\x00\x00\x00\x00 Xpsd;/dev/flash0;/dev/flash1;/" 0 0x80000020 # off=0 addr=0x80000020 size=<whole file>
  ```
  
- WRITE\_REGISTER `<address>` `<value>` `<format 8/16/32>`
  
  Description: Host sends WRITE\_REGISTER command to device, to write value to the adress specified in the ADRESS field. PSD uses this command to handle action.
  
  Addresses use in PSD:
  
  |COMMAND | ADDRESS | VALUE |
  | --- | --- | --- |
  |CHANGE\_PARTITION|-1|file number|
  |ERASE\_PARTITION\_ADDRESS |-2 |size to erase since the beginning of partition (or 0 for full partition)|
  |ERASE\_CHIP\_ADDRESS |-3|size to erase since the beginning of chip (or 0 for full chip)|
  |CHECK\_PRODUCTION |-4|none|
  |CONTROL\_BLOCK\_ADDRESS|-5|1 - FCB; 2 - DBBT|
  |BLOW\_FUSES|-6|none|
  |CLOSE\_PSD|-100|0|
  
  Example:

  ```
  WRITE_REGISTER -1 0 8 # switches file to file 0
  ```

  ```
  WRITE_REGISTER -4 1 8 # flash FCB
  ```

- JUMP\_ADDRESS \<address\>
  
  Description: The device jumps to the adress specified in the ADDRESS field.
  
- ERROR\_STATUS
  
  Description: When the device receives the ERROR\_STATUS command, it returns the global error status that is updated for each command.
