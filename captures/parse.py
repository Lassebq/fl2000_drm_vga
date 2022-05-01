import json
import sys

filename = sys.argv[1]
usbaddr = sys.argv[2]

registers = open('../fl2000_registers.h')

registermaps = {}
registernames = {}
registervalue = {}
registerseen = {}
defines = {}

curmap = 0
curoffset = 0
for line in registers.readlines():
    if line.startswith("#define"):
        tokens = line.split(" ")
        if "+" in line:
            name = tokens[1]
            offset = defines[tokens[2].split("(")[1]] + int(tokens[4].split(")")[0],16)
            registermaps[offset] = []
            registernames[offset] = name
            registervalue[offset] = 0
            registerseen[offset] = 0
            curmap = offset
        else:
            if len(tokens) > 2 and tokens[2].startswith("0x"):
                defines[tokens[1]] = int(tokens[2], 16)
    elif ':' in line and not line.startswith("/*") and ';' in line:
        tokens = line.split(":")
        name = tokens[0].split(" ")[-2]
        bitlength = tokens[1].split(" ")[1].split(";")[0]
        registermaps[curmap].append((name, int(bitlength)))

capture = json.load(open(filename))

def getfields(address, value):
    maps = []
    if registerseen[address]:
        changed = value ^ registervalue[address]
    else:
        changed = 0
        registerseen[address] = 1
    registervalue[address] = value
    for field in registermaps[address]:
        fieldvalue = value & (2**field[1]-1)
        fieldchanged = changed & (2**field[1]-1)
        maps.append((field[0], fieldvalue, fieldchanged))
        value >>= field[1]
        changed >>= field[1]
    return maps

def handle_packet(time, direction, address, value):
    if direction:
        print(time + " WR 0x{:04x} ".format(address) + value)
    else:
        print(time + " RD 0x{:04x} ".format(address) + value)
    fields = getfields(address,int(value,16))
    print("  " + registernames[address])
    for field in fields:
        print("    " + field[0].ljust(25) + " = " + str(field[1]).ljust(6) + " " + hex(field[1]).ljust(6) + (" ***CHANGED***" if field[2] else ""))
rd_data = ""
for packet in capture:
    if packet["_source"]["layers"]["usb"]["usb.src"] != usbaddr and packet["_source"]["layers"]["usb"]["usb.dst"] != usbaddr:
        continue
    if packet["_source"]["layers"]["usb"]["usb.transfer_type"] != "0x02":
        continue
    if "usb.control.Response" in packet["_source"]["layers"]:
        handle_packet(packet["_source"]["layers"]["frame"]["frame.time"], 0, rd_data, "".join(packet["_source"]["layers"]["usb.control.Response"].split(":")[::-1]))
    if "Setup Data" not in packet["_source"]["layers"]:
        continue
    request = packet["_source"]["layers"]["Setup Data"]
    if packet["_source"]["layers"]["Setup Data"]["usb.bmRequestType"] == "0xc0":
        rd_data = int(request["usb.setup.wIndex"])
    if packet["_source"]["layers"]["Setup Data"]["usb.bmRequestType"] == "0x40" and "usb.data_fragment" in request:
        handle_packet(packet["_source"]["layers"]["frame"]["frame.time"], 1, int(request["usb.setup.wIndex"]), "".join(request["usb.data_fragment"].split(":")[::-1]))