"""
Tibia 7.4 .dat parser — extracts item→sprite mappings for the PS2 POC map.
Confirmed format:
  Header: uint32 sig + 4x uint16 (items, outfits, effects, missiles) = 12 bytes
  Each thing:
    attrs: u8 codes until 0xFF (no data)
    layout: width(u8), height(u8), [realsize(u8) if >1x1],
            layers(u8), px(u8), py(u8), pz(u8), phases(u8)
    sprites: uint16 * (width*height*layers*px*py*pz*phases)
"""
import struct

with open('Tibia.dat', 'rb') as f:
    data = f.read()

sig = struct.unpack_from('<I', data, 0)[0]
item_count, outfit_count, effect_count, missile_count = struct.unpack_from('<HHHH', data, 4)

pos = [12]

def u8():
    v = data[pos[0]]; pos[0] += 1
    return v

def u16():
    v = struct.unpack_from('<H', data, pos[0])[0]; pos[0] += 2
    return v

def parse_thing():
    attrs = []
    while True:
        a = u8()
        if a == 0xFF: break
        attrs.append(a)
    w = u8(); h = u8()
    if w > 1 or h > 1: u8()
    layers = u8(); px = u8(); py = u8(); pz = u8(); phases = u8()
    total = w * h * layers * px * py * pz * phases
    sprites = [u16() for _ in range(total)]
    return {'attrs': attrs, 'w': w, 'h': h, 'layers': layers,
            'px': px, 'py': py, 'pz': pz, 'phases': phases, 'sprites': sprites}

# Parse items
items = []
for i in range(item_count):
    items.append(parse_thing())

print(f'=== PARSED {len(items)} ITEMS ===\n')

# Categorize useful items
print('--- SIMPLE GROUND TILES (1 sprite, 1x1, fully covers tile) ---')
grounds = []
for i, item in enumerate(items):
    iid = 100 + i
    if (item['w'] == 1 and item['h'] == 1 and item['layers'] == 1 and
        item['px'] == 1 and item['py'] == 1 and item['phases'] == 1 and
        len(item['sprites']) == 1 and item['sprites'][0] > 0):
        grounds.append((iid, item['sprites'][0], item['attrs']))
        if len(grounds) <= 20:
            print(f'  Item {iid}: sprite {item["sprites"][0]}, attrs={item["attrs"]}')

print(f'\n--- WALL CANDIDATES (items with "blocks" attrs, 1x1, simple) ---')
# In 7.4, attr 11 = NotWalkable (file code), attr 12 = NotMoveable
walls = []
for i, item in enumerate(items):
    iid = 100 + i
    if (item['w'] == 1 and item['h'] == 1 and item['layers'] == 1 and
        item['phases'] == 1 and len(item['sprites']) <= 2 and
        any(a in item['attrs'] for a in [11, 12, 14, 15])):
        walls.append((iid, item['sprites'], item['attrs']))
        if len(walls) <= 15:
            print(f'  Item {iid}: sprites={item["sprites"][:2]}, attrs={item["attrs"]}')

print(f'\n--- ITEMS (stackable/pickupable, 1x1, small) ---')
stackables = []
for i, item in enumerate(items):
    iid = 100 + i
    # attr 14 = stackable (file code 14→shifted 15=Stackable), attr 16 = pickupable?
    if (item['w'] == 1 and item['h'] == 1 and item['layers'] == 1 and
        item['px'] == 1 and item['py'] == 1 and item['phases'] <= 4 and
        len(item['sprites']) <= 4 and
        14 in item['attrs']):
        stackables.append((iid, item['sprites'], item['attrs']))
        if len(stackables) <= 15:
            print(f'  Item {iid}: sprites={item["sprites"]}, attrs={item["attrs"]}')

# Parse first N outfits to find creatures
print(f'\n--- FIRST 15 OUTFITS (creatures) ---')
for i in range(min(15, outfit_count)):
    try:
        outfit = parse_thing()
        oid = 1 + i
        # For a static POC, we want direction 0, phase 0 = first sprite
        first_sprite = outfit['sprites'][0] if outfit['sprites'] else 0
        print(f'  Outfit {oid}: {outfit["w"]}x{outfit["h"]}, layers={outfit["layers"]}, '
              f'px={outfit["px"]}, py={outfit["py"]}, pz={outfit["pz"]}, '
              f'phases={outfit["phases"]}, first_sprite={first_sprite}, '
              f'sprites={outfit["sprites"][:4]}')
    except Exception as e:
        print(f'  Outfit {1+i}: ERROR at pos {pos[0]}: {e}')
        break

print(f'\nFinal pos: {pos[0]} / {len(data)}')
