# Internal PE32 inspection shared by build and release checks.
import struct


class PE:
    def __init__(self, data):
        self.data = data
        if data[:2] != b'MZ':
            raise ValueError('Not a PE image')
        offset = self.unpack('<I', 0x3c)[0]
        if data[offset:offset + 4] != b'PE\0\0':
            raise ValueError('Missing PE signature')
        self.machine, count = self.unpack('<HH', offset + 4)
        optional_size, self.characteristics = self.unpack('<HH', offset + 20)
        self.optional = offset + 24
        if self.unpack('<H', self.optional)[0] != 0x10b:
            raise ValueError('Expected a PE32 image')
        self.sections = [self.unpack('<IIII', self.optional + optional_size + index * 40 + 8)
                         for index in range(count)]

    def unpack(self, format_string, offset):
        return struct.unpack_from(format_string, self.data, offset)

    def offset(self, address):
        for virtual_size, virtual_address, raw_size, raw_pointer in self.sections:
            if virtual_address <= address < virtual_address + min(virtual_size, raw_size):
                return raw_pointer + address - virtual_address
        raise ValueError('RVA outside file-backed sections: 0x%x' % address)

    def string(self, address):
        start = self.offset(address)
        return self.data[start:self.data.index(b'\0', start)].decode('ascii')

    def directory(self, index):
        return self.unpack('<II', self.optional + 96 + index * 8)

    def exports(self):
        address, _ = self.directory(0)
        if not address:
            return set()
        offset = self.offset(address)
        count = self.unpack('<I', offset + 24)[0]
        names = self.unpack('<I', offset + 32)[0]
        return {self.string(self.unpack('<I', self.offset(names) + index * 4)[0])
                for index in range(count)}

    def imports(self):
        address, size = self.directory(1)
        if not address:
            return set()
        result = set()
        for displacement in range(0, size, 20):
            descriptor = self.unpack('<IIIII', self.offset(address + displacement))
            if not any(descriptor):
                return result
            result.add(self.string(descriptor[3]).lower())
        raise ValueError('Unterminated import directory')


def inspect_dll(path, required=()):
    image = PE(path.read_bytes())
    if image.machine != 0x14c or not image.characteristics & 0x2000:
        raise ValueError('Expected an i386 DLL: ' + str(path))
    exports, imports = image.exports(), image.imports()
    if missing := set(required) - exports:
        raise ValueError('Missing exports in %s: %s' % (path, sorted(missing)))
    return {'machine': 'i386', 'exports': sorted(exports), 'imports': sorted(imports)}
