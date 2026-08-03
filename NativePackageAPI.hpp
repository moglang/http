#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXPR_HOST_API_ABI_VERSION 2u
#define EXPR_NATIVE_PACKAGE_ABI_VERSION 3u
#define EXPR_NATIVE_PACKAGE_NAMESPACED_ABI_VERSION 2u

typedef enum ExprPackageValueKind {
    EXPR_PACKAGE_VALUE_NULL = 0,
    EXPR_PACKAGE_VALUE_BOOL = 1,
    EXPR_PACKAGE_VALUE_I64 = 2,
    EXPR_PACKAGE_VALUE_U64 = 3,
    EXPR_PACKAGE_VALUE_F64 = 4,
    EXPR_PACKAGE_VALUE_STR = 5,
    EXPR_PACKAGE_VALUE_HANDLE = 6,
    EXPR_PACKAGE_VALUE_BYTES = 7,
    EXPR_PACKAGE_VALUE_REF = 8,
} ExprPackageValueKind;

typedef struct ExprPersistentValue ExprPersistentValue;
typedef struct ExprPackageValueRef ExprPackageValueRef;

typedef struct ExprPackageStringView {
    const char* data;
    size_t length;
} ExprPackageStringView;

// Byte views are borrowed for the duration of a native call. The VM copies a
// returned byte view into a GC-owned Array<u8> before the callback returns to
// interpreted code.
typedef struct ExprPackageByteView {
    const uint8_t* data;
    size_t length;
} ExprPackageByteView;

typedef void (*ExprPackageHandleFinalizer)(void* handle_data);

typedef struct ExprPackageHandleValue {
    const char* package_namespace;
    const char* package_name;
    const char* type_name;
    void* handle_data;
    ExprPackageHandleFinalizer finalizer;
} ExprPackageHandleValue;

typedef struct ExprPackageValue {
    ExprPackageValueKind kind;
    union {
        bool boolean_value;
        int64_t i64_value;
        uint64_t u64_value;
        double f64_value;
        ExprPackageStringView string_value;
        ExprPackageByteView bytes_value;
        ExprPackageHandleValue handle_value;
        const ExprPackageValueRef* ref_value;
    } as;
} ExprPackageValue;

typedef bool (*ExprHostRetainValueFn)(void* context,
                                      const ExprPackageValue* borrowed_value,
                                      ExprPersistentValue** out_persistent,
                                      ExprPackageStringView* out_error);
typedef void (*ExprHostReleaseValueFn)(void* context,
                                       ExprPersistentValue* persistent);
typedef bool (*ExprHostGetValueFn)(void* context,
                                   ExprPersistentValue* persistent,
                                   ExprPackageValue* out_borrowed_value,
                                   ExprPackageStringView* out_error);
typedef bool (*ExprHostInvokeValueFn)(void* context,
                                      ExprPersistentValue* persistent_callable,
                                      const ExprPackageValue* args, size_t argc,
                                      ExprPackageValue* out_result,
                                      ExprPackageStringView* out_error);

typedef struct ExprHostApi {
    uint32_t abi_version;
    size_t struct_size;
    void* context;
    ExprHostRetainValueFn retainValue;
    ExprHostReleaseValueFn releaseValue;
    ExprHostGetValueFn getValue;
    ExprHostInvokeValueFn invokeValue;
} ExprHostApi;

typedef bool (*ExprNativePackageFn)(const ExprHostApi* host_api,
                                    const ExprPackageValue* args, size_t argc,
                                    ExprPackageValue* out_result,
                                    ExprPackageStringView* out_error);

typedef struct ExprPackageFunctionExport {
    const char* name;
    const char* signature;
    int arity;
    ExprNativePackageFn callback;
} ExprPackageFunctionExport;

typedef struct ExprPackageConstantExport {
    const char* name;
    const char* type_name;
    ExprPackageValue value;
} ExprPackageConstantExport;

typedef struct ExprPackageRegistrationHeader {
    uint32_t abi_version;
} ExprPackageRegistrationHeader;

typedef struct ExprPackageRegistration {
    uint32_t abi_version;
    const char* package_namespace;
    const char* package_name;
    const ExprPackageFunctionExport* functions;
    size_t function_count;
    const ExprPackageConstantExport* constants;
    size_t constant_count;
} ExprPackageRegistration;

typedef const ExprPackageRegistration* (*ExprRegisterPackageFn)(void);
