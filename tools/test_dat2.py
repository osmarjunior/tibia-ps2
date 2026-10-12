import struct
import sys

with open('Tibia.dat', 'rb') as f:
    data = f.read()

sig = struct.unpack_from('<I', data, 0)[0]
item_count, outfit_count, effect_count, missile_count = struct.unpack_from('<HHHH', data, 4)
print(f'Items: {item_count}, Outfits: {outfit_count}, Effects: {effect_count}, Missiles: {missile_count}')

pos = [12]  # mutable

def u8():
    v = data[pos[0]]; pos[0] += 1
    return v

def u16():
    v = struct.unpack_from('<H', data, pos[0])[0]; pos[0] += 2
    return v

def parse_thing(read_attr_data):
    # Attributes
    while True:
        attr = u8()
        if attr == 0xFF:
            break
        if read_attr_data:
            # TBD: which attrs read data
            pass
    # Layout (6 or 7 bytes)
    width = u8()
    height = u8()
    realsize = u8() if (width > 1 or height > 1) else None
    layers = u8()
    px = u8()
    py = u8()
    pz = u8()  # try always reading patternZ
    phases = u8()

    total = width * height * layers * px * py * pz * phases
    sprites = [u16() for _ in range(total)]
    return width, height, layers, px, py, pz, phases, sprites

# Test with different configurations
for desc, use_7byte, read_attr in [
    ('6-byte layout, no attr data', False, False),
    ('7-byte layout (always pz), no attr data', True, False),
    ('6-byte layout, attr code 1 reads u16', False, {1: 2}),
    ('7-byte layout, attr code 1 reads u16', True, {1: 2}),
    ('6-byte layout, attr 1,16 read data', False, {1: 2, 16: 4}),
    ('7-byte layout, attr 1,16 read data', True, {1: 2, 16: 4}),
]:
    pos[0] = 12
    parsed = 0
    oob = 0
    try:
        for i in range(item_count):
            # attributes
            while True:
                attr = u8()
                if attr == 0xFF: break
                if read_attr and attr in read_attr:
                    pos[0] += read_attr[attr]
            # layout
            width = u8(); height = u8()
            if width > 1 or height > 1: u8()  # realsize
            layers = u8(); px = u8(); py = u8()
            if use_7byte: pz = u8()
            else: pz = 1
            phases = u8()
            total = width * height * layers * px * py * pz * phases
            for _ in range(total):
                sid = u16()
                if sid > 9999: oob += 1
            parsed += 1
    except:
        pass
    print(f'{desc}: parsed {parsed}/{item_count}, pos {pos[0]}/{len(data)}, oob={oob}')
