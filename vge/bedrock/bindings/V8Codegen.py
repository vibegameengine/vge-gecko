import os
import re
import sys

import buildconfig

ROOT_INTERFACES = [
    "Window",
    "Document",
    "HTMLDocument",
    "DocumentFragment",
    "Text",
    "Comment",
    "CSSStyleProperties",
    "DOMTokenList",
    "NodeList",
    "HTMLCollection",
    "DOMRect",
    "Attr",
    "NamedNodeMap",
    "TrustedHTML",
    "TrustedScript",
    "TrustedScriptURL",
    "Event",
    "UIEvent",
    "MouseEvent",
    "PointerEvent",
    "WheelEvent",
    "KeyboardEvent",
    "FocusEvent",
    "InputEvent",
    "CustomEvent",
    "HTMLAnchorElement",
    "HTMLBodyElement",
    "HTMLBRElement",
    "HTMLButtonElement",
    "HTMLCanvasElement",
    "HTMLDivElement",
    "HTMLHeadElement",
    "HTMLHeadingElement",
    "HTMLHtmlElement",
    "HTMLImageElement",
    "HTMLInputElement",
    "HTMLLabelElement",
    "HTMLLIElement",
    "HTMLLinkElement",
    "HTMLMetaElement",
    "HTMLOListElement",
    "HTMLParagraphElement",
    "HTMLPreElement",
    "HTMLScriptElement",
    "HTMLSpanElement",
    "HTMLStyleElement",
    "HTMLTableCellElement",
    "HTMLTableElement",
    "HTMLTableRowElement",
    "HTMLTitleElement",
    "HTMLUListElement",
]

PAGE_SCRIPT_TRUE_FUNCS = {
    "IsNotUAWidget",
}

EVENT_HANDLER_TYPES = {
    "EventHandlerNonNull",
    "OnErrorEventHandlerNonNull",
    "OnBeforeUnloadEventHandlerNonNull",
}

CUSTOM_METHODS = {
    ("EventTarget", "addEventListener"): ("EventTarget_addEventListener", 2),
    ("EventTarget", "removeEventListener"): ("EventTarget_removeEventListener", 2),
    ("Window", "setTimeout"): ("Window_setTimeout", 1),
    ("Window", "setInterval"): ("Window_setInterval", 1),
    ("Window", "clearTimeout"): ("Window_clearTimeout", 0),
    ("Window", "clearInterval"): ("Window_clearInterval", 0),
    ("Window", "requestAnimationFrame"): ("Window_requestAnimationFrame", 1),
    ("Window", "cancelAnimationFrame"): ("Window_cancelAnimationFrame", 1),
    ("Window", "queueMicrotask"): ("Window_queueMicrotask", 1),
}

EVENT_CASTS = [
    ("PointerEvent", "AsPointerEvent"),
    ("KeyboardEvent", "AsKeyboardEvent"),
    ("CustomEvent", "AsCustomEvent"),
]

EVENT_CLASSES = [
    ("WheelEvent", "eWheelEventClass"),
    ("MouseEvent", "eMouseEventClass"),
    ("MouseEvent", "eDragEventClass"),
    ("FocusEvent", "eFocusEventClass"),
    ("InputEvent", "eEditorInputEventClass"),
    ("UIEvent", "eUIEventClass"),
]

INTEGER_TAGS = {
    "byte": "int8_t",
    "octet": "uint8_t",
    "short": "int16_t",
    "unsigned short": "uint16_t",
    "long": "int32_t",
    "unsigned long": "uint32_t",
    "long long": "int64_t",
    "unsigned long long": "uint64_t",
}


def setup_imports():
    srcdir = buildconfig.topsrcdir
    for path in ("dom/bindings", "dom/bindings/parser"):
        full = os.path.join(srcdir, path)
        if full not in sys.path:
            sys.path.insert(0, full)


class Unsupported(Exception):
    pass


CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const",
    "continue", "default", "delete", "do", "double", "else", "enum", "explicit", "export", "extern", "false",
    "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "not",
    "operator", "or", "private", "protected", "public", "register", "return", "short", "signed", "sizeof",
    "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "while", "xor",
}


def atomName(name):
    return "_" + name if name in CPP_KEYWORDS else name


def cid(name):
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


class Generator:
    def __init__(self, config):
        from Codegen import (
            CGSpecializedGetterCommon,
            CGSpecializedMethod,
            CGSpecializedSetterCommon,
            getRetvalDeclarationForType,
        )

        self.config = config
        self.getterName = CGSpecializedGetterCommon.makeNativeName
        self.setterName = CGSpecializedSetterCommon.makeNativeName
        self.methodName = CGSpecializedMethod.makeNativeName
        self.retval = getRetvalDeclarationForType
        self.skipped = []
        self.unionHeaders = set()
        self.references = set()
        self.descriptors = self.collect()
        self.protoIndex = {d.interface.identifier.name: i for i, d in enumerate(self.descriptors)}

    def collect(self):
        names = []
        seen = set()

        def add(name):
            if name in seen:
                return
            descriptor = self.config.getDescriptor(name)
            if descriptor.interface.parent:
                add(descriptor.interface.parent.identifier.name)
            seen.add(name)
            names.append(name)

        for name in ROOT_INTERFACES:
            add(name)
        for descriptor in self.config.getDescriptors(
            hasInterfacePrototypeObject=True,
            isCallback=False,
            isJSImplemented=False,
            isExposedInWindow=True,
        ):
            if "<" in descriptor.nativeType:
                continue
            if descriptor.interface.getExtendedAttribute("ChromeOnly"):
                continue
            add(descriptor.interface.identifier.name)
        return [self.config.getDescriptor(n) for n in names]

    def has(self, name):
        return name in self.protoIndex

    def descriptorFor(self, name):
        return self.descriptors[self.protoIndex[name]]

    def protoConst(self, name):
        return "kProto_%s" % name

    def interfaceTypeName(self, idlType):
        inner = idlType.unroll().inner
        name = inner.identifier.name
        if not self.has(name):
            raise Unsupported("interface %s is not bound" % name)
        if name == "WindowProxy":
            raise Unsupported("WindowProxy")
        self.references.add(name)
        return name

    def checkArgType(self, t):
        if t.isPrimitive() and (t.tag() in self.primitiveTags()):
            return
        if t.isDOMString() or t.isUSVString() or t.isUTF8String():
            return
        if t.isGeckoInterface() and not t.isCallback():
            self.interfaceTypeName(t)
            return
        if t.isUnion() and not t.nullable():
            for member in t.flatMemberTypes:
                if member.isDictionary():
                    continue
                if member.isDOMString() or member.isUSVString():
                    continue
                if member.isGeckoInterface() and not member.isCallback() and self.has(member.inner.identifier.name):
                    continue
                raise Unsupported("union member %s in %s" % (member, t))
            return
        raise Unsupported("argument type %s" % t)

    def checkReturnType(self, t):
        if t is None or t.isUndefined():
            return
        if t.isPrimitive() and (t.tag() in self.primitiveTags()):
            return
        if t.isDOMString() or t.isUSVString() or t.isByteString() or t.isUTF8String():
            return
        if t.isGeckoInterface() and t.unroll().inner.identifier.name == "WindowProxy":
            return
        if t.isGeckoInterface() and not t.isCallback():
            self.interfaceTypeName(t)
            return
        raise Unsupported("return type %s" % t)

    def primitiveTags(self):
        from WebIDL import IDLType

        return {
            IDLType.Tags.bool: "bool",
            IDLType.Tags.int8: "int8_t",
            IDLType.Tags.uint8: "uint8_t",
            IDLType.Tags.int16: "int16_t",
            IDLType.Tags.uint16: "uint16_t",
            IDLType.Tags.int32: "int32_t",
            IDLType.Tags.uint32: "uint32_t",
            IDLType.Tags.int64: "int64_t",
            IDLType.Tags.uint64: "uint64_t",
            IDLType.Tags.float: "float",
            IDLType.Tags.unrestricted_float: "float",
            IDLType.Tags.double: "double",
            IDLType.Tags.unrestricted_double: "double",
        }

    def checkNeedsCx(self, returnType, arguments, attrs):
        from Codegen import needCx

        if needCx(returnType, arguments, attrs, considerTypes=True):
            raise Unsupported("needs JSContext")

    def checkExtended(self, descriptor, member, attrs):
        if member.getExtendedAttribute("ChromeOnly"):
            raise Unsupported("ChromeOnly")
        func = member.getExtendedAttribute("Func")
        if func and func[0] not in PAGE_SCRIPT_TRUE_FUNCS:
            raise Unsupported("Func=%s" % func[0])
        if "implicitJSContext" in attrs:
            raise Unsupported("implicitJSContext")
        if member.isStatic():
            raise Unsupported("static")
        if (member.isMethod() and member.isMaplikeOrSetlikeOrIterableMethod()) or (
            member.isAttr() and member.isMaplikeOrSetlikeAttr()
        ):
            raise Unsupported("maplike/setlike/iterable member")

    def pref(self, member):
        pref = member.getExtendedAttribute("Pref")
        if pref:
            return '"%s"' % pref[0]
        return "nullptr"

    def convertVariadic(self, where, t, index, name):
        from Codegen import CGUnionStruct, getUnionMemberName

        if t.nullable():
            raise Unsupported("nullable variadic")
        slot = "slot"
        value = "info[i]"
        if t.isUnion():
            unionName = CGUnionStruct.unionTypeName(t, True)
            self.unionHeaders.add(self.unionHeader(t))
            elemType = unionName
            convert = []
            for member in t.flatMemberTypes:
                if member.isGeckoInterface():
                    iface = self.interfaceTypeName(member)
                    native = self.descriptorFor(iface).nativeType
                    convert.append(
                        "if (auto* p = static_cast<%s*>(Unwrap(isolate, %s, %s))) { %s.SetAs%s() = p; } else"
                        % (native, value, self.protoConst(iface), slot, getUnionMemberName(member))
                    )
            strings = [m for m in t.flatMemberTypes if m.isDOMString() or m.isUSVString()]
            if not strings:
                raise Unsupported("variadic union without a string member")
            convert.append(
                "if (!ToString(isolate, %s, %s.SetAs%s(), StringNull::Stringify)) return;"
                % (value, slot, getUnionMemberName(strings[0]))
            )
        elif t.isDOMString() or t.isUSVString():
            elemType = "nsString"
            convert = ["if (!ToString(isolate, %s, %s, StringNull::Stringify)) return;" % (value, slot)]
        elif t.isGeckoInterface():
            iface = self.interfaceTypeName(t)
            native = self.descriptorFor(iface).nativeType
            elemType = "OwningNonNull<%s>" % native
            convert = [
                "auto* p = static_cast<%s*>(Unwrap(isolate, %s, %s));" % (native, value, self.protoConst(iface)),
                'if (!p) { ThrowTypeError(isolate, "%s: argument is not a %s"); return; }' % (where, iface),
                "%s = p;" % slot,
            ]
        else:
            raise Unsupported("variadic %s" % t)
        lines = [
            "Sequence<%s> %s;" % (elemType, name),
            "for (int i = %d; i < info.Length(); ++i) {" % index,
            "  %s* slotPtr = %s.AppendElement(mozilla::fallible);" % (elemType, name),
            '  if (!slotPtr) { ThrowTypeError(isolate, "%s: out of memory"); return; }' % where,
            "  %s& %s = *slotPtr;" % (elemType, slot),
        ]
        lines += ["  " + l for l in convert]
        lines.append("}")
        return lines, name

    def convertArg(self, where, t, index, name, optional, default, variadic):
        if variadic:
            return self.convertVariadic(where, t, index, name)
        lines = []
        value = "info[%d]" % index
        missing = "info.Length() <= %d || info[%d]->IsUndefined()" % (index, index)
        tags = self.primitiveTags()
        if t.isPrimitive():
            ctype = tags[t.tag()]
            if t.nullable():
                raise Unsupported("nullable primitive argument")
            if optional and default is None:
                lines.append("Optional<%s> %s;" % (ctype, name))
                lines.append("if (!(%s)) {" % missing)
                lines.append("  %s tmp;" % ctype)
                lines += ["  " + l for l in self.primitiveFrom(t, ctype, value, "tmp", where)]
                lines.append("  %s.Construct(tmp);" % name)
                lines.append("}")
                return lines, name
            lines.append("%s %s = %s;" % (ctype, name, self.primitiveDefault(t, default)))
            guard = "if (!(%s)) {" % missing if optional else "{"
            lines.append(guard)
            lines += ["  " + l for l in self.primitiveFrom(t, ctype, value, name, where)]
            lines.append("}")
            return lines, name
        if t.isDOMString() or t.isUSVString() or t.isUTF8String():
            nullMode = "StringNull::Stringify"
            if t.nullable():
                nullMode = "StringNull::Null"
            elif t.legacyNullToEmptyString:
                nullMode = "StringNull::EmptyString"
            wide = not t.isUTF8String()
            stringType = "nsAutoString" if wide else "nsAutoCString"
            abstractType = "nsAString" if wide else "nsACString"
            if optional and default is None:
                lines.append("Optional<%s> %s;" % (abstractType, name))
                lines.append("%s %s_holder;" % (stringType, name))
                lines.append("if (!(%s)) {" % missing)
                lines.append(
                    "  if (!ToString(isolate, %s, %s_holder, %s)) return;"
                    % (value, name, nullMode)
                )
                lines.append("  %s = &%s_holder;" % (name, name))
                lines.append("}")
                return lines, name
            lines.append("%s %s;" % (stringType, name))
            if optional:
                lines.append("if (%s) {" % missing)
                lines += ["  " + l for l in self.stringDefault(name, default, narrow=not wide)]
                lines.append("} else if (!ToString(isolate, %s, %s, %s)) return;" % (value, name, nullMode))
            else:
                lines.append("if (!ToString(isolate, %s, %s, %s)) return;" % (value, name, nullMode))
            return lines, name
        if t.isUnion():
            return self.convertUnion(where, t, index, name, optional, default, value, missing), name
        if t.isGeckoInterface():
            iface = self.interfaceTypeName(t)
            native = self.descriptorFor(iface).nativeType
            if optional and not t.nullable():
                raise Unsupported("optional non-nullable interface argument")
            lines.append("%s* %s = nullptr;" % (native, name))
            lines.append("if (!(info.Length() <= %d || info[%d]->IsNullOrUndefined())) {" % (index, index))
            lines.append(
                "  %s = static_cast<%s*>(Unwrap(isolate, %s, %s));"
                % (name, native, value, self.protoConst(iface))
            )
            lines.append("  if (!%s) { ThrowTypeError(isolate, \"%s: argument %d is not a %s\"); return; }" % (name, where, index + 1, iface))
            lines.append("}")
            if t.nullable():
                return lines, name
            lines.append("if (!%s) { ThrowTypeError(isolate, \"%s: argument %d is not a %s\"); return; }" % (name, where, index + 1, iface))
            return lines, "*%s" % name
        raise Unsupported("argument %s" % t)

    def convertUnion(self, where, t, index, name, optional, default, value, missing):
        from Codegen import CGUnionStruct, getUnionMemberName
        from WebIDL import IDLDefaultDictionaryValue

        if optional and default is None:
            raise Unsupported("optional union without default")
        unionName = CGUnionStruct.unionTypeName(t, False)
        self.unionHeaders.add(self.unionHeader(t))
        lines = ["%s %s;" % (unionName, name)]
        dictionaries = [m for m in t.flatMemberTypes if m.isDictionary()]
        strings = [m for m in t.flatMemberTypes if m.isDOMString() or m.isUSVString()]
        interfaces = [m for m in t.flatMemberTypes if m.isGeckoInterface()]
        if optional:
            if not isinstance(default, IDLDefaultDictionaryValue) or not dictionaries:
                raise Unsupported("union default %s" % default)
            dictName = dictionaries[0].inner.identifier.name
            lines.append("if (%s) {" % missing)
            lines.append(
                "  static_cast<%s&>(%s.RawSetAs%s()) = %s();"
                % (dictName, name, getUnionMemberName(dictionaries[0]), dictName)
            )
            lines.append("} else {")
            indent = "  "
        else:
            indent = ""
        body = []
        for member in interfaces:
            iface = member.inner.identifier.name
            self.references.add(iface)
            native = self.descriptorFor(iface).nativeType
            body.append(
                "if (auto* p = static_cast<%s*>(Unwrap(isolate, %s, %s))) { %s.RawSetAs%s() = p; } else"
                % (native, value, self.protoConst(iface), name, getUnionMemberName(member))
            )
        if dictionaries:
            body.append(
                'if (%s->IsObject()) { ThrowTypeError(isolate, "%s: dictionary arguments are not supported yet"); return; } else'
                % (value, where)
            )
        if strings:
            body.append(
                "if (!ToString(isolate, %s, %s.RawSetAs%s(), StringNull::Stringify)) return;"
                % (value, name, getUnionMemberName(strings[0]))
            )
        else:
            body.append('{ ThrowTypeError(isolate, "%s: argument %d has the wrong type"); return; }' % (where, index + 1))
        lines += [indent + l for l in body]
        if optional:
            lines.append("}")
        return lines

    def unionHeader(self, t):
        from Codegen import CGHeaders

        return CGHeaders.getUnionDeclarationFilename(self.config, t)

    def primitiveDefault(self, t, default):
        if default is None:
            return "false" if self.primitiveTags()[t.tag()] == "bool" else "0"
        value = default.value
        if isinstance(value, bool):
            return "true" if value else "false"
        return repr(value)

    def stringDefault(self, name, default, narrow=False):
        if default is None:
            return []
        from WebIDL import IDLNullValue

        if isinstance(default, IDLNullValue):
            return ["%s.SetIsVoid(true);" % name]
        text = default.value
        if not text:
            return ["%s.Truncate();" % name]
        escaped = text.replace("\\", "\\\\").replace('"', '\\"')
        if narrow:
            return ['%s.Assign("%s");' % (name, escaped)]
        return ['%s.Assign(NS_ConvertUTF8toUTF16("%s"));' % (name, escaped)]

    def primitiveFrom(self, t, ctype, value, out, where):
        if ctype == "bool":
            return ["%s = ToBoolean(isolate, %s);" % (out, value)]
        if ctype in ("float", "double"):
            lines = ["double %s_number;" % out]
            if t.isUnrestricted():
                lines.append("if (!ToNumber(isolate, %s, &%s_number)) return;" % (value, out))
            else:
                lines.append('if (!ToRestrictedDouble(isolate, %s, &%s_number, "%s")) return;' % (value, out, where))
            lines.append("%s = static_cast<%s>(%s_number);" % (out, ctype, out))
            return lines
        return ["if (!ToInteger<%s>(isolate, %s, &%s)) return;" % (ctype, value, out)]

    def returnConversion(self, t, var):
        if t is None or t.isUndefined():
            return []
        if t.isPrimitive():
            ctype = self.primitiveTags()[t.tag()]
            get = var
            lines = []
            if t.nullable():
                lines.append("if (%s.IsNull()) { info.GetReturnValue().SetNull(); return; }" % var)
                get = "%s.Value()" % var
            if ctype == "bool":
                lines.append("info.GetReturnValue().Set(static_cast<bool>(%s));" % get)
            elif ctype in ("int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t"):
                lines.append("info.GetReturnValue().Set(static_cast<int32_t>(%s));" % get)
            elif ctype == "uint32_t":
                lines.append("info.GetReturnValue().Set(static_cast<uint32_t>(%s));" % get)
            else:
                lines.append("info.GetReturnValue().Set(static_cast<double>(%s));" % get)
            return lines
        if t.isDOMString() or t.isUSVString():
            return ["info.GetReturnValue().Set(FromString(isolate, %s));" % var]
        if t.isByteString() or t.isUTF8String():
            return ["info.GetReturnValue().Set(FromUTF8String(isolate, %s));" % var]
        if t.isGeckoInterface() and t.unroll().inner.identifier.name == "WindowProxy":
            return ["info.GetReturnValue().Set(WrapWindowProxy(isolate, %s));" % var]
        if t.isGeckoInterface():
            return ["info.GetReturnValue().Set(Wrap(isolate, GetRaw(%s)));" % var]
        raise Unsupported("return %s" % t)

    def bindingTemplate(self, descriptor, member):
        template = member.getExtendedAttribute("BindingTemplate")
        if template is None:
            return None, []
        templateName, templateType, additionalArg = template[0][0], template[0][1], template[0][2]
        candidate = self.config.attributeTemplates.get(templateName)
        if candidate is None:
            raise Unsupported("binding template %s not found" % templateName)
        return candidate, ["%s::%s" % (templateType, additionalArg or member.identifier.name)]

    def callLines(self, descriptor, member, nativeName, returnType, argExprs, attrs, where, preArgs=()):
        result, outParam, _, resultArgs, conversion = self.retval(returnType, descriptor)
        args = list(preArgs) + list(argExprs)
        lines = []
        resultVar = None
        if result is not None:
            resultVar = "result"
        if outParam == "ref":
            lines.append("%s result;" % result.define())
            args.append("result")
        elif outParam is not None:
            raise Unsupported("pointer out param")
        if "needsSubjectPrincipal" in attrs:
            if "needsNonSystemSubjectPrincipal" in attrs:
                lines.append("nsIPrincipal* subjectPrincipal = NonSystemSubjectPrincipal(isolate);")
                args.append("subjectPrincipal")
            else:
                lines.append("nsIPrincipal* subjectPrincipal = SubjectPrincipal(isolate);")
                lines.append('if (!subjectPrincipal) { ThrowTypeError(isolate, "%s: no subject principal"); return; }' % where)
                args.append("*subjectPrincipal")
        if member.getExtendedAttribute("NeedsCallerType"):
            args.append("CallerType::NonSystem")
        fallible = "needsErrorResult" in attrs or "canOOM" in attrs
        if "needsErrorResult" in attrs:
            args.append("rv")
        elif "canOOM" in attrs:
            args.append("OOMReporter::From(rv)")
        if fallible:
            lines.insert(0, "binding_detail::FastErrorResult rv;")
        call = "self->%s(%s)" % (nativeName, ", ".join(args))
        if result is None or outParam is not None:
            lines.append("%s;" % call)
        elif conversion is not None:
            lines.append("auto result(%s(%s));" % (conversion.define(), call))
        else:
            lines.append("%s result(%s);" % (result.define(), call))
        if fallible:
            lines.append('if (rv.Failed()) { ThrowErrorResult(isolate, rv, "%s"); return; }' % where)
        lines += self.returnConversion(returnType, "result")
        return lines

    def receiver(self, descriptor, where):
        return [
            "v8::Isolate* isolate = info.GetIsolate();",
            "auto* self = static_cast<%s*>(Unwrap(isolate, info.This(), %s));"
            % (descriptor.nativeType, self.protoConst(descriptor.interface.identifier.name)),
            'if (!self) { ThrowTypeError(isolate, "%s: illegal invocation"); return; }' % where,
        ]

    def genMethod(self, descriptor, member, out):
        iface = descriptor.interface.identifier.name
        name = member.identifier.name
        where = "%s.%s" % (iface, name)
        attrs = descriptor.getExtendedAttributes(member)
        self.checkExtended(descriptor, member, attrs)
        signatures = member.signatures()
        if len(signatures) != 1:
            raise Unsupported("overloaded")
        returnType, arguments = signatures[0]
        self.checkNeedsCx(returnType, arguments, attrs)
        self.checkReturnType(returnType)
        body = self.receiver(descriptor, where)
        required = sum(1 for a in arguments if not a.optional and not a.variadic)
        if required:
            body.append('if (info.Length() < %d) { ThrowTypeError(isolate, "%s: not enough arguments"); return; }' % (required, where))
        exprs = []
        for index, arg in enumerate(arguments):
            if not arg.variadic:
                self.checkArgType(arg.type)
            lines, expr = self.convertArg(where, arg.type, index, "arg%d" % index, arg.optional, arg.defaultValue, arg.variadic)
            body += lines
            exprs.append(expr)
        body += self.callLines(descriptor, member, self.methodName(descriptor, member), returnType, exprs, attrs, where)
        fn = "Method_%s_%s" % (cid(iface), cid(name))
        out.append("static void %s(const v8::FunctionCallbackInfo<v8::Value>& info) {" % fn)
        out += ["  " + l for l in body]
        out.append("}")
        return fn, required

    def genGetter(self, descriptor, member, out):
        iface = descriptor.interface.identifier.name
        name = member.identifier.name
        where = "%s.%s" % (iface, name)
        attrs = descriptor.getExtendedAttributes(member, getter=True)
        self.checkExtended(descriptor, member, attrs)
        self.checkNeedsCx(member.type, [], attrs)
        self.checkReturnType(member.type)
        body = self.receiver(descriptor, where)
        template, preArgs = self.bindingTemplate(descriptor, member)
        nativeName = template.getter if template else self.getterName(descriptor, member)
        body += self.callLines(descriptor, member, nativeName, member.type, [], attrs, where, preArgs)
        fn = "Get_%s_%s" % (cid(iface), cid(name))
        out.append("static void %s(const v8::FunctionCallbackInfo<v8::Value>& info) {" % fn)
        out += ["  " + l for l in body]
        out.append("}")
        return fn

    def isEventHandler(self, member):
        t = member.type
        return t.isCallback() and t.unroll().callback.identifier.name in EVENT_HANDLER_TYPES

    def genEventHandler(self, descriptor, member, out):
        iface = descriptor.interface.identifier.name
        name = member.identifier.name
        if not name.startswith("on"):
            raise Unsupported("event handler without on prefix")
        eventType = name[2:]
        receiver = self.receiver(descriptor, "%s.%s" % (iface, name))
        getter = "GetHandler_%s_%s" % (cid(iface), cid(name))
        setter = "SetHandler_%s_%s" % (cid(iface), cid(name))
        out.append("static void %s(const v8::FunctionCallbackInfo<v8::Value>& info) {" % getter)
        out += ["  " + l for l in receiver]
        out.append('  info.GetReturnValue().Set(GetEventHandler(isolate, static_cast<EventTarget*>(self), u"%s"_ns));' % eventType)
        out.append("}")
        out.append("static void %s(const v8::FunctionCallbackInfo<v8::Value>& info) {" % setter)
        out += ["  " + l for l in receiver]
        out.append('  SetEventHandler(isolate, static_cast<EventTarget*>(self), u"%s"_ns, info[0]);' % eventType)
        out.append("}")
        return getter, setter

    def genIndexedGetter(self, descriptor, out):
        operation = descriptor.operations["IndexedGetter"]
        if operation is None:
            return None
        iface = descriptor.interface.identifier.name
        where = "%s[index]" % iface
        returnType, arguments = operation.signatures()[0]
        self.checkReturnType(returnType)
        attrs = descriptor.getExtendedAttributes(operation)
        self.checkExtended(descriptor, operation, attrs)
        nativeName = descriptor.binaryNameFor("IndexedGetter", False)
        fn = "Indexed_%s" % cid(iface)
        body = [
            "v8::Isolate* isolate = info.GetIsolate();",
            "auto* self = static_cast<%s*>(Unwrap(isolate, info.Holder(), %s));"
            % (descriptor.nativeType, self.protoConst(iface)),
            "if (!self) return;",
            "bool found = false;",
        ]
        call = self.callLines(descriptor, operation, nativeName, returnType, ["index", "found"], attrs, where)
        conversion = self.returnConversion(returnType, "result")
        callOnly = [l for l in call if l not in conversion]
        body += callOnly
        body.append("if (!found) return;")
        body += conversion
        out.append("static void %s(uint32_t index, const v8::PropertyCallbackInfo<v8::Value>& info) {" % fn)
        out += ["  " + l for l in body]
        out.append("}")
        return fn

    def genSetter(self, descriptor, member, out):
        if member.readonly:
            return None
        iface = descriptor.interface.identifier.name
        name = member.identifier.name
        where = "%s.%s" % (iface, name)
        attrs = descriptor.getExtendedAttributes(member, setter=True)
        self.checkExtended(descriptor, member, attrs)
        self.checkNeedsCx(None, [member], attrs)
        self.checkArgType(member.type)
        body = self.receiver(descriptor, where)
        lines, expr = self.convertArg(where, member.type, 0, "arg0", False, None, False)
        body += lines
        template, preArgs = self.bindingTemplate(descriptor, member)
        nativeName = template.setter if template else self.setterName(descriptor, member)
        body += self.callLines(descriptor, member, nativeName, None, [expr], attrs, where, preArgs)
        fn = "Set_%s_%s" % (cid(iface), cid(name))
        out.append("static void %s(const v8::FunctionCallbackInfo<v8::Value>& info) {" % fn)
        out += ["  " + l for l in body]
        out.append("}")
        return fn

    def genInterface(self, descriptor, out):
        iface = descriptor.interface.identifier.name
        install = []
        for member in descriptor.interface.members:
            name = member.identifier.name
            try:
                if member.isConst():
                    value = member.value.value
                    literal = ("true" if value else "false") if isinstance(value, bool) else repr(value)
                    install.append(
                        '  { auto v = v8::Number::New(isolate, static_cast<double>(%s)); tmpl->Set(isolate, "%s", v, kConstAttrs); proto->Set(isolate, "%s", v, kConstAttrs); }'
                        % (literal, name, name)
                    )
                elif member.isMethod() and (iface, name) in CUSTOM_METHODS:
                    fn, length = CUSTOM_METHODS[(iface, name)]
                    install.append(
                        '  proto->Set(isolate, "%s", v8::FunctionTemplate::New(isolate, %s, {}, {}, %d));'
                        % (name, fn, length)
                    )
                elif member.isAttr() and self.isEventHandler(member):
                    getter, setter = self.genEventHandler(descriptor, member, out)
                    install.append(
                        '  %s->SetAccessorProperty(v8::String::NewFromUtf8Literal(isolate, "%s"), v8::FunctionTemplate::New(isolate, %s), v8::FunctionTemplate::New(isolate, %s));'
                        % ("proto", name, getter, setter)
                    )
                elif member.isMethod():
                    if member.isStringifier() and not member.signatures()[0][1] and name == "__stringifier":
                        raise Unsupported("stringifier")
                    if not name or name.startswith("__"):
                        raise Unsupported("special operation")
                    fn, length = self.genMethod(descriptor, member, out)
                    install.append(
                        '  if (InstallMember(%s)) proto->Set(isolate, "%s", v8::FunctionTemplate::New(isolate, %s, {}, {}, %d));'
                        % (self.pref(member), name, fn, length)
                    )
                elif member.isAttr():
                    if member.isStatic():
                        raise Unsupported("static attribute")
                    getter = self.genGetter(descriptor, member, out)
                    setter = None
                    try:
                        setter = self.genSetter(descriptor, member, out)
                    except Unsupported as e:
                        self.skipped.append("%s.%s (setter): %s" % (iface, name, e))
                    setterTemplate = (
                        "v8::FunctionTemplate::New(isolate, %s)" % setter
                        if setter
                        else "v8::Local<v8::FunctionTemplate>()"
                    )
                    target = "inst" if member.getExtendedAttribute("LegacyUnforgeable") else "proto"
                    names = [name] + list(member.getExtendedAttribute("BindingAlias") or [])
                    for exposedName in names:
                        install.append(
                            '  if (InstallMember(%s)) %s->SetAccessorProperty(v8::String::NewFromUtf8Literal(isolate, "%s"), v8::FunctionTemplate::New(isolate, %s), %s);'
                            % (self.pref(member), target, exposedName, getter, setterTemplate)
                        )
            except Unsupported as e:
                self.skipped.append("%s.%s: %s" % (iface, name, e))

        try:
            indexed = self.genIndexedGetter(descriptor, out)
            if indexed:
                install.append("  inst->SetHandler(v8::IndexedPropertyHandlerConfiguration(%s));" % indexed)
        except Unsupported as e:
            self.skipped.append("%s[index]: %s" % (iface, e))

        parent = descriptor.interface.parent
        out.append("v8::Local<v8::FunctionTemplate> Template_%s(v8::Isolate* isolate) {" % cid(iface))
        out.append("  v8::Local<v8::FunctionTemplate> tmpl = CachedTemplate(isolate, %s);" % self.protoConst(iface))
        out.append("  if (!tmpl.IsEmpty()) return tmpl;")
        out.append("  tmpl = v8::FunctionTemplate::New(isolate, IllegalConstructor);")
        out.append('  tmpl->SetClassName(v8::String::NewFromUtf8Literal(isolate, "%s"));' % iface)
        out.append("  tmpl->InstanceTemplate()->SetInternalFieldCount(2);")
        if parent:
            out.append("  tmpl->Inherit(Template_%s(isolate));" % cid(parent.identifier.name))
        out.append("  StoreTemplate(isolate, %s, tmpl);" % self.protoConst(iface))
        out.append("  v8::Local<v8::ObjectTemplate> proto = tmpl->PrototypeTemplate();")
        out.append("  v8::Local<v8::ObjectTemplate> inst = tmpl->InstanceTemplate();")
        out.append("  (void)inst;")
        out += install
        out.append("  return tmpl;")
        out.append("}")

    def genCasts(self, descriptor, out):
        iface = descriptor.interface.identifier.name
        native = descriptor.nativeType
        out.append("void* Cast_%s(int target, void* p) {" % cid(iface))
        out.append("  auto* self = static_cast<%s*>(p);" % native)
        out.append("  switch (target) {")
        for ancestor in descriptor.prototypeChain:
            ancestorNative = self.descriptorFor(ancestor).nativeType
            out.append("    case %s: return static_cast<%s*>(self);" % (self.protoConst(ancestor), ancestorNative))
        out.append("  }")
        out.append("  return nullptr;")
        out.append("}")
        out.append("void AddRef_%s(void* p) { AddRefNative<%s>(p); }" % (cid(iface), native))
        out.append("void Release_%s(void* p) { ReleaseNative<%s>(p); }" % (cid(iface), native))

    def isNodeInterface(self, descriptor):
        return "Node" in descriptor.prototypeChain

    def genNodeDispatch(self, out):
        has = self.has
        out.append("int DynamicNodeProto(nsINode* aNode) {")
        out.append("  switch (aNode->NodeType()) {")
        out.append("    case nsINode::TEXT_NODE: return %s;" % self.protoConst("Text") if has("Text") else "")
        out.append("    case nsINode::COMMENT_NODE: return %s;" % self.protoConst("Comment") if has("Comment") else "")
        out.append("    case nsINode::DOCUMENT_FRAGMENT_NODE: return %s;" % self.protoConst("DocumentFragment"))
        out.append("    case nsINode::DOCUMENT_NODE:")
        out.append("      return aNode->AsDocument()->IsHTMLDocument() ? %s : %s;" % (self.protoConst("HTMLDocument"), self.protoConst("Document")))
        out.append("    case nsINode::ELEMENT_NODE: break;")
        out.append("    default: return %s;" % self.protoConst("Node"))
        out.append("  }")
        out.append("  mozilla::dom::Element* element = aNode->AsElement();")
        out.append("  if (element->IsHTMLElement()) {")
        out.append("    nsAtom* tag = element->NodeInfo()->NameAtom();")
        out.append("    (void)tag;")
        for tag, iface in self.htmlTags():
            if has(iface):
                out.append("    if (tag == nsGkAtoms::%s) return %s;" % (atomName(tag), self.protoConst(iface)))
        out.append("    return %s;" % self.protoConst("HTMLElement"))
        out.append("  }")
        out.append("  return %s;" % self.protoConst("Element"))
        out.append("}")
        out.append("void* NodeAsProto(nsINode* aNode, int aProto) {")
        out.append("  switch (aProto) {")
        for d in self.descriptors:
            if self.isNodeInterface(d):
                out.append("    case %s: return static_cast<%s*>(aNode);" % (self.protoConst(d.interface.identifier.name), d.nativeType))
        out.append("  }")
        out.append("  return nullptr;")
        out.append("}")

    def genEventDispatch(self, out):
        out.append("int DynamicEventProto(mozilla::dom::Event* aEvent) {")
        for iface, cast in EVENT_CASTS:
            if self.has(iface):
                out.append("  if (aEvent->%s()) return %s;" % (cast, self.protoConst(iface)))
        out.append("  switch (aEvent->WidgetEventPtr()->mClass) {")
        for iface, eventClass in EVENT_CLASSES:
            if self.has(iface):
                out.append("    case %s: return %s;" % (eventClass, self.protoConst(iface)))
        out.append("    default: break;")
        out.append("  }")
        out.append("  return %s;" % self.protoConst("Event"))
        out.append("}")
        out.append("void* EventAsProto(mozilla::dom::Event* aEvent, int aProto) {")
        out.append("  switch (aProto) {")
        for d in self.descriptors:
            if "Event" in d.prototypeChain:
                out.append("    case %s: return static_cast<%s*>(aEvent);" % (self.protoConst(d.interface.identifier.name), d.nativeType))
        out.append("  }")
        out.append("  return nullptr;")
        out.append("}")

    def htmlTags(self):
        path = os.path.join(buildconfig.topsrcdir, "parser", "htmlparser", "nsHTMLTagList.inc")
        tags = []
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                m = re.match(r"HTML_TAG\((\w+),\s*(\w+),\s*(\w+)\)", line.strip())
                if m:
                    tags.append((m.group(1), "HTML%sElement" % m.group(3)))
        return tags

    def headers(self):
        result = set()
        for d in self.descriptors:
            result.add(d.headerFile)
        return sorted(result)

    COMMON_INCLUDES = [
        "vge/bedrock/VgeV8BindingsGenerated.h",
        "mozilla/dom/BindingDeclarations.h",
        "mozilla/dom/Document.h",
        "mozilla/dom/Element.h",
        "mozilla/dom/Event.h",
        "mozilla/BasicEvents.h",
        "vge/bedrock/VgeV8Events.h",
        "nsGkAtoms.h",
        "nsGlobalWindowInner.h",
    ]

    def generateHeader(self):
        out = [
            "#ifndef vge_bedrock_VgeV8BindingsGenerated_h",
            "#define vge_bedrock_VgeV8BindingsGenerated_h",
            '#include "vge/bedrock/VgeV8BindingSupport.h"',
            "namespace vge::bedrock::v8b {",
            "enum : int {",
        ]
        for d in self.descriptors:
            out.append("  %s," % self.protoConst(d.interface.identifier.name))
        out.append("  kProtoCount")
        out.append("};")
        out.append("static constexpr v8::PropertyAttribute kConstAttrs = static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);")
        for d in self.descriptors:
            name = cid(d.interface.identifier.name)
            out.append("v8::Local<v8::FunctionTemplate> Template_%s(v8::Isolate* isolate);" % name)
            out.append("void* Cast_%s(int target, void* p);" % name)
            out.append("void AddRef_%s(void* p);" % name)
            out.append("void Release_%s(void* p);" % name)
        out.append("}")
        out.append("#endif")
        return "\n".join(out) + "\n"

    def includeLines(self, headers):
        lines = []
        for header in sorted(set(headers)):
            if "/" not in header and header.endswith("Binding.h"):
                header = "mozilla/dom/" + header
            lines.append('#include "%s"' % header)
        return lines

    def protoIdLines(self, names):
        lines = []
        seen = set()
        for name in sorted(names):
            d = self.descriptorFor(name)
            if d.nativeType in seen or self.isNodeInterface(d) or "Event" in d.prototypeChain:
                continue
            seen.add(d.nativeType)
            lines.append(
                "template <> struct ProtoIdOf<%s> { static constexpr int kId = %s; };"
                % (d.nativeType, self.protoConst(name))
            )
        return lines

    def generateShard(self, descriptors):
        self.references = set()
        self.unionHeaders = set()
        body = []
        for d in descriptors:
            self.genCasts(d, body)
            body.append("")
            self.genInterface(d, body)
            body.append("")
        names = set(self.references)
        for d in descriptors:
            names.update(d.prototypeChain)
        headers = [self.descriptorFor(n).headerFile for n in names] + list(self.unionHeaders)
        out = ['#include "%s"' % h for h in self.COMMON_INCLUDES]
        out += self.includeLines(headers)
        out += ["", "using namespace mozilla;", "using namespace mozilla::dom;", "", "namespace vge::bedrock::v8b {", "using DOMString = mozilla::dom::DOMString;"]
        out += self.protoIdLines(names)
        out.append("")
        out += body
        out.append("}")
        return "\n".join(out) + "\n"

    def generateMain(self):
        out = ['#include "%s"' % h for h in self.COMMON_INCLUDES]
        dispatched = [d for d in self.descriptors if self.isNodeInterface(d) or "Event" in d.prototypeChain]
        out += self.includeLines([d.headerFile for d in dispatched])
        out += ["", "using namespace mozilla;", "using namespace mozilla::dom;", "", "namespace vge::bedrock::v8b {", "using DOMString = mozilla::dom::DOMString;"]
        out.append("const ProtoInfo gProtoInfos[] = {")
        for d in self.descriptors:
            name = cid(d.interface.identifier.name)
            out.append('  {"%s", Cast_%s, AddRef_%s, Release_%s, Template_%s},' % (name, name, name, name, name))
        out.append("};")
        out.append("const int gProtoCount = kProtoCount;")
        out.append("int WindowProto() { return %s; }" % self.protoConst("Window"))
        self.genNodeDispatch(out)
        self.genEventDispatch(out)
        out.append("}")
        return "\n".join(line for line in out if line is not None) + "\n"

    def shards(self, count):
        groups = [[] for _ in range(count)]
        for index, d in enumerate(self.descriptors):
            groups[index % count].append(d)
        return groups


def parse_configuration():
    import json

    import WebIDL
    from Configuration import Configuration

    bindings_objdir = os.path.join(buildconfig.topobjdir, "dom", "bindings")
    with open(os.path.join(bindings_objdir, "file-lists.json"), encoding="utf-8") as fh:
        files = json.load(fh)
    cache_dir = os.path.join(buildconfig.topobjdir, "vge", "bedrock", "bindings", "_cache")
    os.makedirs(cache_dir, exist_ok=True)
    parser = WebIDL.Parser(cache_dir, lexer=None)
    for path in sorted(files["webidls"]):
        with open(path, encoding="utf-8") as fh:
            parser.parse(fh.read(), path)
    results = parser.finish()
    roots = sorted({os.path.dirname(path) for path in files["webidls"]})
    return Configuration(
        os.path.join(buildconfig.topsrcdir, "dom", "bindings", "Bindings.conf"),
        roots,
        results,
        files["generated_events_stems"],
    )


SHARD_COUNT = 24


def main(output, *inputs):
    setup_imports()
    generator = Generator(parse_configuration())
    directory = os.path.dirname(output.name)
    for index, group in enumerate(generator.shards(SHARD_COUNT)):
        with open(os.path.join(directory, "VgeV8Bindings%d.cpp" % index), "w", encoding="utf-8") as fh:
            fh.write(generator.generateShard(group))
    with open(os.path.join(directory, "VgeV8BindingsGenerated.h"), "w", encoding="utf-8") as fh:
        fh.write(generator.generateHeader())
    output.write(generator.generateMain())
    with open(os.path.join(directory, "VgeV8Bindings.skipped.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(generator.skipped) + "\n")
    return set()
