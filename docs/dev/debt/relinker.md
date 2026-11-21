# relinker technical debt

### Functional

- [Conversion](../../../core/relinker/relinker/src/Conversion.cpp) (relinker) - an image whose headers have no room for another section header is refused instead of having its sections moved
- [Conversion](../../../core/relinker/relinker/src/Conversion.cpp) (relinker) - imports by ordinal are routed by DLL name, but AnyBox libraries export by name only, so an ordinal import from a routed DLL fails at load
- [Conversion](../../../core/relinker/relinker/src/Conversion.cpp) (relinker) - Console-specific load-config, TLS and DLL characteristics fields are kept unchanged; whether any of them needs translating is unknown
