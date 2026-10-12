import struct

with open('Tibia.dat', 'rb') as f:
    data = f.read()

# Header: uint32 sig + 4x uint16 = 12 bytes
sig = struct.unpack_from('<I', data, 0)[0]
item_count, outfit_count, effect_count, missile_count = struct.unpack_from('<HHHH', data, 4)
print(f'Sig: 0x{sig:08X}, Items: {item_count}, Outfits: {outfit_count}, Effects: {effect_count}, Missiles: {missile_count}')

pos = 12

def read_u8():
    global pos
    v = data[pos]; pos += 1
    return v

def read_u16():
    global pos
    v = struct.unpack_from('<H', data, pos)[0]; pos += 2
    return v

def parse_thing():
    """Parse one thing from the .dat file."""
    # Read attribute codes until 0xFF
    # For 7.4: file codes 1-15 shift +1, some read data
    while True:
        attr = read_u8()
        if attr == 0xFF:
            break
        # Attributes that read extra data in 7.4:
        if attr == 16:  # Light: 2x uint16
            read_u16(); read_u16()
        elif attr == 19:  # Elevation: uint16
            read_u16()
        elif attr == 22:  # MinimapColor: uint16
            read_u16()
        elif 1 <= attr <= 15:
            # Shifted attributes: code+1
            shifted = attr + 1
            # Which shifted attrs read data?
            # From otclient: Writable(8), WritableOnce(9), Cloth(?), LensHelp(?), DefaultAction(?)
            # Ground(3) reads uint16 speed
            if shifted == 3:   # Ground: reads uint16 speed
                read_u16()
            elif shifted == 8:  # Writable: reads uint16
                read_u16()
            elif shifted == 9:  # WritableOnce: reads uint16
                read_u16()

    # Layout: 7 bytes (width, height, extra, layers, px, py, phases)
    # The "extra" byte might be realSize or patternZ - always present in 7.4
    width = read_u8()
    height = read_u8()
    if width > 1 or height > 1:
        _realsize = read_u8()  # only for multi-tile
    extra = read_u8()  # mystery byte - always present in 7.4
    layers = read_u8()
    px = read_u8()
    py = read_u8()
    # patternZ NOT in file for < 755
    phases = read_u8()

    total_sprites = width * height * layers * px * py * 1 * phases
    sprites = []
    for _ in range(total_sprites):
        sprites.append(read_u16())

    return {
        'width': width, 'height': height, 'extra': extra,
        'layers': layers, 'px': px, 'py': py, 'phases': phases,
        'sprites': sprites
    }

# Parse all items
items = []
errors = 0
for i in range(item_count):
    try:
        item = parse_thing()
        items.append(item)
        # Sanity check: sprite IDs should be < 9999
        for sid in item['sprites']:
            if sid > 9999:
                print(f'WARNING: item {100+i} has out-of-range sprite {sid}')
                errors += 1
                break
    except Exception as e:
        print(f'Error at item {100+i}, pos {pos}: {e}')
        break

print(f'\nParsed {len(items)} items')
print(f'Final position: {pos} / {len(data)}')
print(f'Out-of-range sprites: {errors}')

# Show some items
print('\n--- Ground items (1x1, 1 layer, 1x1 pattern) ---')
count = 0
for i, item in enumerate(items):
    if item['width'] == 1 and item['height'] == 1 and item['layers'] == 1 and item['px'] == 1 and item['py'] == 1 and len(item['sprites']) <= 2:
        if count < 20:
            print(f'  Item {100+i}: sprites={item["sprites"]}')
            count += 1

print('\n--- Items with 4x4 pattern (ground borders) ---')
count = 0
for i, item in enumerate(items):
    if item['px'] == 4 and item['py'] == 4:
        if count < 5:
            print(f'  Item {100+i}: {item["width"]}x{item["height"]}, layers={item["layers"]}, sprites={item["sprites"][:4]}...')
            count += 1
