// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.
// http://code.google.com/p/protobuf/
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Author: jonskeet@google.com (Jon Skeet)
//  Following the Java generator by kenton@google.com (Kenton Varda)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.

#include <algorithm>
#include <google/protobuf/stubs/hash.h>
#include <google/protobuf/compiler/csharp/csharp_message.h>
#include <google/protobuf/compiler/csharp/csharp_enum.h>
#include <google/protobuf/compiler/csharp/csharp_extension.h>
#include <google/protobuf/compiler/csharp/csharp_helpers.h>
#include <google/protobuf/stubs/strutil.h>
#include <google/protobuf/io/printer.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format.h>
#include <google/protobuf/descriptor.pb.h>

namespace google {
namespace protobuf {
namespace compiler {
namespace csharp {

using internal::WireFormat;

namespace {

void PrintFieldComment(io::Printer* printer, const FieldDescriptor* field) {
  // Print the field's proto-syntax definition as a comment.  We don't want to
  // print group bodies so we cut off after the first line.
  string def = field->DebugString();
  printer->Print("// $def$\r\n",
    "def", def.substr(0, def.find_first_of('\n')));
}

struct FieldOrderingByNumber {
  inline bool operator()(const FieldDescriptor* a,
                         const FieldDescriptor* b) const {
    return a->number() < b->number();
  }
};

struct ExtensionRangeOrdering {
  bool operator()(const Descriptor::ExtensionRange* a,
                  const Descriptor::ExtensionRange* b) const {
    return a->start < b->start;
  }
};

// Sort the fields of the given Descriptor by number into a new[]'d array
// and return it.
const FieldDescriptor** SortFieldsByNumber(const Descriptor* descriptor) {
  const FieldDescriptor** fields =
    new const FieldDescriptor*[descriptor->field_count()];
  for (int i = 0; i < descriptor->field_count(); i++) {
    fields[i] = descriptor->field(i);
  }
  sort(fields, fields + descriptor->field_count(),
       FieldOrderingByNumber());
  return fields;
}

// Get an identifier that uniquely identifies this type within the file.
// This is used to declare static variables related to this type at the
// outermost file scope.
string UniqueFileScopeIdentifier(const Descriptor* descriptor) {
  return "static_" + StringReplace(descriptor->full_name(), ".", "_", true);
}

// Returns true if the message type has any required fields.  If it doesn't,
// we can optimize out calls to its isInitialized() method.
//
// already_seen is used to avoid checking the same type multiple times
// (and also to protect against recursion).
static bool HasRequiredFields(
    const Descriptor* type,
    hash_set<const Descriptor*>* already_seen) {
  if (already_seen->count(type) > 0) {
    // The type is already in cache.  This means that either:
    // a. The type has no required fields.
    // b. We are in the midst of checking if the type has required fields,
    //    somewhere up the stack.  In this case, we know that if the type
    //    has any required fields, they'll be found when we return to it,
    //    and the whole call to HasRequiredFields() will return true.
    //    Therefore, we don't have to check if this type has required fields
    //    here.
    return false;
  }
  already_seen->insert(type);

  // If the type has extensions, an extension with message type could contain
  // required fields, so we have to be conservative and assume such an
  // extension exists.
  if (type->extension_range_count() > 0) return true;

  for (int i = 0; i < type->field_count(); i++) {
    const FieldDescriptor* field = type->field(i);
    if (field->is_required()) {
      return true;
    }
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      if (HasRequiredFields(field->message_type(), already_seen)) {
        return true;
      }
    }
  }

  return false;
}

static bool HasRequiredFields(const Descriptor* type) {
  hash_set<const Descriptor*> already_seen;
  return HasRequiredFields(type, &already_seen);
}

}  // namespace

// ===================================================================

MessageGenerator::MessageGenerator(const Descriptor* descriptor)
  : descriptor_(descriptor),
    field_generators_(descriptor) {
}

MessageGenerator::~MessageGenerator() {}

void MessageGenerator::GenerateStaticVariables(io::Printer* printer) {
  // Because descriptor.proto (com.google.protobuf.DescriptorProtos) is
  // used in the construction of descriptors, we have a tricky bootstrapping
  // problem.  To help control static initialization order, we make sure all
  // descriptors and other static data that depends on them are members of
  // the outermost class in the file.  This way, they will be initialized in
  // a deterministic order.

  map<string, string> vars;
  vars["identifier"] = UniqueFileScopeIdentifier(descriptor_);
  vars["index"] = SimpleItoa(descriptor_->index());
  vars["classname"] = ClassName(descriptor_);
  if (descriptor_->containing_type() != NULL) {
    vars["parent"] = UniqueFileScopeIdentifier(descriptor_->containing_type());
  }
  if (descriptor_->file()->options().csharp_multiple_files()) {
    // We can only make these package-private since the classes that use them
    // are in separate files.
    vars["private"] = "";
  } else {
    vars["private"] = "private ";
  }

  // The descriptor for this type.
  if (descriptor_->containing_type() == NULL) {
    printer->Print(vars,
      "$private$static final com.google.protobuf.Descriptors.Descriptor\r\n"
      "  internal_$identifier$_descriptor =\r\n"
      "    getDescriptor().getMessageTypes().get($index$);\r\n");
  } else {
    printer->Print(vars,
      "$private$static final com.google.protobuf.Descriptors.Descriptor\r\n"
      "  internal_$identifier$_descriptor =\r\n"
      "    internal_$parent$_descriptor.getNestedTypes().get($index$);\r\n");
  }

  // And the FieldAccessorTable.
  printer->Print(vars,
    "$private$static\r\n"
    "  com.google.protobuf.GeneratedMessage.FieldAccessorTable\r\n"
    "    internal_$identifier$_fieldAccessorTable = new\r\n"
    "      com.google.protobuf.GeneratedMessage.FieldAccessorTable(\r\n"
    "        internal_$identifier$_descriptor,\r\n"
    "        new string[] { ");
  for (int i = 0; i < descriptor_->field_count(); i++) {
    printer->Print(
      "\"$field_name$\", ",
      "field_name",
        UnderscoresToCapitalizedCamelCase(descriptor_->field(i)));
  }
  printer->Print("},\r\n"
    "        $classname$.class,\r\n"
    "        $classname$.Builder.class);\r\n",
    "classname", ClassName(descriptor_));

  // Generate static members for all nested types.
  for (int i = 0; i < descriptor_->nested_type_count(); i++) {
    // TODO(kenton):  Reuse MessageGenerator objects?
    MessageGenerator(descriptor_->nested_type(i))
      .GenerateStaticVariables(printer);
  }
}

void MessageGenerator::Generate(io::Printer* printer) {
  bool is_own_file =
    descriptor_->containing_type() == NULL &&
    descriptor_->file()->options().csharp_multiple_files();

  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
      "public $static$ final class $classname$ extends\r\n"
      "    com.google.protobuf.GeneratedMessage.ExtendableMessage<\r\n"
      "      $classname$> {\r\n",
      "static", is_own_file ? "" : "static",
      "classname", descriptor_->name());
  } else {
    printer->Print(
      "public $static$ final class $classname$ extends\r\n"
      "    com.google.protobuf.GeneratedMessage {\r\n",
      "static", is_own_file ? "" : "static",
      "classname", descriptor_->name());
  }
  printer->Indent();
  printer->Print(
    "// Use $classname$.newBuilder() to construct.\r\n"
    "private $classname$() {}\r\n"
    "\r\n"
    "private static final $classname$ defaultInstance = new $classname$();\r\n"
    "public static $classname$ getDefaultInstance() {\r\n"
    "  return defaultInstance;\r\n"
    "}\r\n"
    "\r\n"
    "public $classname$ getDefaultInstanceForType() {\r\n"
    "  return defaultInstance;\r\n"
    "}\r\n"
    "\r\n",
    "classname", descriptor_->name());
  printer->Print(
    "public static final com.google.protobuf.Descriptors.Descriptor\r\n"
    "    getDescriptor() {\r\n"
    "  return $fileclass$.internal_$identifier$_descriptor;\r\n"
    "}\r\n"
    "\r\n"
    "protected com.google.protobuf.GeneratedMessage.FieldAccessorTable\r\n"
    "    internalGetFieldAccessorTable() {\r\n"
    "  return $fileclass$.internal_$identifier$_fieldAccessorTable;\r\n"
    "}\r\n"
    "\r\n",
    "fileclass", ClassName(descriptor_->file()),
    "identifier", UniqueFileScopeIdentifier(descriptor_));

  // Nested types and extensions
  for (int i = 0; i < descriptor_->enum_type_count(); i++) {
    EnumGenerator(descriptor_->enum_type(i)).Generate(printer);
  }

  for (int i = 0; i < descriptor_->nested_type_count(); i++) {
    MessageGenerator(descriptor_->nested_type(i)).Generate(printer);
  }

  for (int i = 0; i < descriptor_->extension_count(); i++) {
    ExtensionGenerator(descriptor_->extension(i)).Generate(printer);
  }

  // Fields
  for (int i = 0; i < descriptor_->field_count(); i++) {
    PrintFieldComment(printer, descriptor_->field(i));
    field_generators_.get(descriptor_->field(i)).GenerateMembers(printer);
    printer->Print("\r\n");
  }

  if (descriptor_->file()->options().optimize_for() == FileOptions::SPEED) {
    GenerateIsInitialized(printer);
    GenerateMessageSerializationMethods(printer);
  }

  GenerateParseFromMethods(printer);
  GenerateBuilder(printer);
}

// ===================================================================

void MessageGenerator::
GenerateMessageSerializationMethods(io::Printer* printer) {
  scoped_array<const FieldDescriptor*> sorted_fields(
    SortFieldsByNumber(descriptor_));

  vector<const Descriptor::ExtensionRange*> sorted_extensions;
  for (int i = 0; i < descriptor_->extension_range_count(); ++i) {
    sorted_extensions.push_back(descriptor_->extension_range(i));
  }
  sort(sorted_extensions.begin(), sorted_extensions.end(),
       ExtensionRangeOrdering());

  printer->Print(
    "public void writeTo(com.google.protobuf.CodedOutputStream output)\r\n"
    "                    throws java.io.IOException {\r\n");
  printer->Indent();

  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
      "com.google.protobuf.GeneratedMessage.ExtendableMessage\r\n"
      "  .ExtensionWriter extensionWriter = newExtensionWriter();\r\n");
  }

  // Merge the fields and the extension ranges, both sorted by field number.
  for (int i = 0, j = 0;
       i < descriptor_->field_count() || j < sorted_extensions.size();
       ) {
    if (i == descriptor_->field_count()) {
      GenerateSerializeOneExtensionRange(printer, sorted_extensions[j++]);
    } else if (j == sorted_extensions.size()) {
      GenerateSerializeOneField(printer, sorted_fields[i++]);
    } else if (sorted_fields[i]->number() < sorted_extensions[j]->start) {
      GenerateSerializeOneField(printer, sorted_fields[i++]);
    } else {
      GenerateSerializeOneExtensionRange(printer, sorted_extensions[j++]);
    }
  }

  if (descriptor_->options().message_set_wire_format()) {
    printer->Print(
      "getUnknownFields().writeAsMessageSetTo(output);\r\n");
  } else {
    printer->Print(
      "getUnknownFields().writeTo(output);\r\n");
  }

  printer->Outdent();
  printer->Print(
    "}\r\n"
    "\r\n"
    "private int memoizedSerializedSize = -1;\r\n"
    "public int getSerializedSize() {\r\n"
    "  int size = memoizedSerializedSize;\r\n"
    "  if (size != -1) return size;\r\n"
    "\r\n"
    "  size = 0;\r\n");
  printer->Indent();

  for (int i = 0; i < descriptor_->field_count(); i++) {
    field_generators_.get(sorted_fields[i]).GenerateSerializedSizeCode(printer);
  }

  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
      "size += extensionsSerializedSize();\r\n");
  }

  if (descriptor_->options().message_set_wire_format()) {
    printer->Print(
      "size += getUnknownFields().getSerializedSizeAsMessageSet();\r\n");
  } else {
    printer->Print(
      "size += getUnknownFields().getSerializedSize();\r\n");
  }

  printer->Outdent();
  printer->Print(
    "  memoizedSerializedSize = size;\r\n"
    "  return size;\r\n"
    "}\r\n"
    "\r\n");
}

void MessageGenerator::
GenerateParseFromMethods(io::Printer* printer) {
  // Note:  These are separate from GenerateMessageSerializationMethods()
  //   because they need to be generated even for messages that are optimized
  //   for code size.
  printer->Print(
    "public static $classname$ parseFrom(\r\n"
    "    com.google.protobuf.ByteString data)\r\n"
    "    throws com.google.protobuf.InvalidProtocolBufferException {\r\n"
    "  return newBuilder().mergeFrom(data).buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(\r\n"
    "    com.google.protobuf.ByteString data,\r\n"
    "    com.google.protobuf.ExtensionRegistry extensionRegistry)\r\n"
    "    throws com.google.protobuf.InvalidProtocolBufferException {\r\n"
    "  return newBuilder().mergeFrom(data, extensionRegistry)\r\n"
    "           .buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(byte[] data)\r\n"
    "    throws com.google.protobuf.InvalidProtocolBufferException {\r\n"
    "  return newBuilder().mergeFrom(data).buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(\r\n"
    "    byte[] data,\r\n"
    "    com.google.protobuf.ExtensionRegistry extensionRegistry)\r\n"
    "    throws com.google.protobuf.InvalidProtocolBufferException {\r\n"
    "  return newBuilder().mergeFrom(data, extensionRegistry)\r\n"
    "           .buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(java.io.InputStream input)\r\n"
    "    throws java.io.IOException {\r\n"
    "  return newBuilder().mergeFrom(input).buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(\r\n"
    "    java.io.InputStream input,\r\n"
    "    com.google.protobuf.ExtensionRegistry extensionRegistry)\r\n"
    "    throws java.io.IOException {\r\n"
    "  return newBuilder().mergeFrom(input, extensionRegistry)\r\n"
    "           .buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(\r\n"
    "    com.google.protobuf.CodedInputStream input)\r\n"
    "    throws java.io.IOException {\r\n"
    "  return newBuilder().mergeFrom(input).buildParsed();\r\n"
    "}\r\n"
    "public static $classname$ parseFrom(\r\n"
    "    com.google.protobuf.CodedInputStream input,\r\n"
    "    com.google.protobuf.ExtensionRegistry extensionRegistry)\r\n"
    "    throws java.io.IOException {\r\n"
    "  return newBuilder().mergeFrom(input, extensionRegistry)\r\n"
    "           .buildParsed();\r\n"
    "}\r\n"
    "\r\n",
    "classname", ClassName(descriptor_));
}

void MessageGenerator::GenerateSerializeOneField(
    io::Printer* printer, const FieldDescriptor* field) {
  field_generators_.get(field).GenerateSerializationCode(printer);
}

void MessageGenerator::GenerateSerializeOneExtensionRange(
    io::Printer* printer, const Descriptor::ExtensionRange* range) {
  printer->Print(
    "extensionWriter.writeUntil($end$, output);\r\n",
    "end", SimpleItoa(range->end));
}

// ===================================================================

void MessageGenerator::GenerateBuilder(io::Printer* printer) {
  printer->Print(
    "public static Builder newBuilder() { return new Builder(); }\r\n"
    "public Builder newBuilderForType() { return new Builder(); }\r\n"
    "public static Builder newBuilder($classname$ prototype) {\r\n"
    "  return new Builder().mergeFrom(prototype);\r\n"
    "}\r\n"
    "\r\n",
    "classname", ClassName(descriptor_));

  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
      "public static final class Builder extends\r\n"
      "    com.google.protobuf.GeneratedMessage.ExtendableBuilder<\r\n"
      "      $classname$, Builder> {\r\n",
      "classname", ClassName(descriptor_));
  } else {
    printer->Print(
      "public static final class Builder extends\r\n"
      "    com.google.protobuf.GeneratedMessage.Builder<Builder> {\r\n",
      "classname", ClassName(descriptor_));
  }
  printer->Indent();

  GenerateCommonBuilderMethods(printer);

  if (descriptor_->file()->options().optimize_for() == FileOptions::SPEED) {
    GenerateBuilderParsingMethods(printer);
  }

  for (int i = 0; i < descriptor_->field_count(); i++) {
    printer->Print("\r\n");
    PrintFieldComment(printer, descriptor_->field(i));
    field_generators_.get(descriptor_->field(i))
                     .GenerateBuilderMembers(printer);
  }

  printer->Outdent();
  printer->Print("}\r\n");
  printer->Outdent();
  printer->Print("}\r\n\r\n");
}

// ===================================================================

void MessageGenerator::GenerateCommonBuilderMethods(io::Printer* printer) {
  printer->Print(
    "// Construct using $classname$.newBuilder()\r\n"
    "private Builder() {}\r\n"
    "\r\n"
    "$classname$ result = new $classname$();\r\n"
    "\r\n"
    "protected $classname$ internalGetResult() {\r\n"
    "  return result;\r\n"
    "}\r\n"
    "\r\n"
    "public Builder clear() {\r\n"
    "  result = new $classname$();\r\n"
    "  return this;\r\n"
    "}\r\n"
    "\r\n"
    "public Builder clone() {\r\n"
    "  return new Builder().mergeFrom(result);\r\n"
    "}\r\n"
    "\r\n"
    "public com.google.protobuf.Descriptors.Descriptor\r\n"
    "    getDescriptorForType() {\r\n"
    "  return $classname$.getDescriptor();\r\n"
    "}\r\n"
    "\r\n"
    "public $classname$ getDefaultInstanceForType() {\r\n"
    "  return $classname$.getDefaultInstance();\r\n"
    "}\r\n"
    "\r\n",
    "classname", ClassName(descriptor_));

  // -----------------------------------------------------------------

  printer->Print(
    "public $classname$ build() {\r\n"
    "  if (!isInitialized()) {\r\n"
    "    throw new com.google.protobuf.UninitializedMessageException(\r\n"
    "      result);\r\n"
    "  }\r\n"
    "  return buildPartial();\r\n"
    "}\r\n"
    "\r\n"
    "private $classname$ buildParsed()\r\n"
    "    throws com.google.protobuf.InvalidProtocolBufferException {\r\n"
    "  if (!isInitialized()) {\r\n"
    "    throw new com.google.protobuf.UninitializedMessageException(\r\n"
    "      result).asInvalidProtocolBufferException();\r\n"
    "  }\r\n"
    "  return buildPartial();\r\n"
    "}\r\n"
    "\r\n"
    "public $classname$ buildPartial() {\r\n",
    "classname", ClassName(descriptor_));
  printer->Indent();

  for (int i = 0; i < descriptor_->field_count(); i++) {
    field_generators_.get(descriptor_->field(i)).GenerateBuildingCode(printer);
  }

  printer->Outdent();
  printer->Print(
    "  $classname$ returnMe = result;\r\n"
    "  result = null;\r\n"
    "  return returnMe;\r\n"
    "}\r\n"
    "\r\n",
    "classname", ClassName(descriptor_));

  // -----------------------------------------------------------------

  if (descriptor_->file()->options().optimize_for() == FileOptions::SPEED) {
    printer->Print(
      "public Builder mergeFrom(com.google.protobuf.Message other) {\r\n"
      "  if (other instanceof $classname$) {\r\n"
      "    return mergeFrom(($classname$)other);\r\n"
      "  } else {\r\n"
      "    super.mergeFrom(other);\r\n"
      "    return this;\r\n"
      "  }\r\n"
      "}\r\n"
      "\r\n"
      "public Builder mergeFrom($classname$ other) {\r\n"
      // Optimization:  If other is the default instance, we know none of its
      //   fields are set so we can skip the merge.
      "  if (other == $classname$.getDefaultInstance()) return this;\r\n",
      "classname", ClassName(descriptor_));
    printer->Indent();

    for (int i = 0; i < descriptor_->field_count(); i++) {
      field_generators_.get(descriptor_->field(i)).GenerateMergingCode(printer);
    }

    printer->Outdent();
    printer->Print(
      "  this.mergeUnknownFields(other.getUnknownFields());\r\n"
      "  return this;\r\n"
      "}\r\n"
      "\r\n");
  }
}

// ===================================================================

void MessageGenerator::GenerateBuilderParsingMethods(io::Printer* printer) {
  scoped_array<const FieldDescriptor*> sorted_fields(
    SortFieldsByNumber(descriptor_));

  printer->Print(
    "public Builder mergeFrom(\r\n"
    "    com.google.protobuf.CodedInputStream input)\r\n"
    "    throws java.io.IOException {\r\n"
    "  return mergeFrom(input,\r\n"
    "    com.google.protobuf.ExtensionRegistry.getEmptyRegistry());\r\n"
    "}\r\n"
    "\r\n"
    "public Builder mergeFrom(\r\n"
    "    com.google.protobuf.CodedInputStream input,\r\n"
    "    com.google.protobuf.ExtensionRegistry extensionRegistry)\r\n"
    "    throws java.io.IOException {\r\n");
  printer->Indent();

  printer->Print(
    "com.google.protobuf.UnknownFieldSet.Builder unknownFields =\r\n"
    "  com.google.protobuf.UnknownFieldSet.newBuilder(\r\n"
    "    this.getUnknownFields());\r\n"
    "while (true) {\r\n");
  printer->Indent();

  printer->Print(
    "int tag = input.readTag();\r\n"
    "switch (tag) {\r\n");
  printer->Indent();

  printer->Print(
    "case 0:\r\n"          // zero signals EOF / limit reached
    "  this.setUnknownFields(unknownFields.build());\r\n"
    "  return this;\r\n"
    "default: {\r\n"
    "  if (!parseUnknownField(input, unknownFields,\r\n"
    "                         extensionRegistry, tag)) {\r\n"
    "    this.setUnknownFields(unknownFields.build());\r\n"
    "    return this;\r\n"   // it's an endgroup tag
    "  }\r\n"
    "  break;\r\n"
    "}\r\n");

  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = sorted_fields[i];
    uint32 tag = WireFormat::MakeTag(field->number(),
      WireFormat::WireTypeForFieldType(field->type()));

    printer->Print(
      "case $tag$: {\r\n",
      "tag", SimpleItoa(tag));
    printer->Indent();

    field_generators_.get(field).GenerateParsingCode(printer);

    printer->Outdent();
    printer->Print(
      "  break;\r\n"
      "}\r\n");
  }

  printer->Outdent();
  printer->Outdent();
  printer->Outdent();
  printer->Print(
    "    }\r\n"     // switch (tag)
    "  }\r\n"       // while (true)
    "}\r\n"
    "\r\n");
}

// ===================================================================

void MessageGenerator::GenerateIsInitialized(io::Printer* printer) {
  printer->Print(
    "public final boolean isInitialized() {\r\n");
  printer->Indent();

  // Check that all required fields in this message are set.
  // TODO(kenton):  We can optimize this when we switch to putting all the
  //   "has" fields into a single bitfield.
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);

    if (field->is_required()) {
      printer->Print(
        "if (!has$name$) return false;\r\n",
        "name", UnderscoresToCapitalizedCamelCase(field));
    }
  }

  // Now check that all embedded messages are initialized.
  for (int i = 0; i < descriptor_->field_count(); i++) {
    const FieldDescriptor* field = descriptor_->field(i);
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE &&
        HasRequiredFields(field->message_type())) {
      switch (field->label()) {
        case FieldDescriptor::LABEL_REQUIRED:
          printer->Print(
            "if (!get$name$().isInitialized()) return false;\r\n",
            "type", ClassName(field->message_type()),
            "name", UnderscoresToCapitalizedCamelCase(field));
          break;
        case FieldDescriptor::LABEL_OPTIONAL:
          printer->Print(
            "if (has$name$()) {\r\n"
            "  if (!get$name$().isInitialized()) return false;\r\n"
            "}\r\n",
            "type", ClassName(field->message_type()),
            "name", UnderscoresToCapitalizedCamelCase(field));
          break;
        case FieldDescriptor::LABEL_REPEATED:
          printer->Print(
            "for ($type$ element : get$name$List()) {\r\n"
            "  if (!element.isInitialized()) return false;\r\n"
            "}\r\n",
            "type", ClassName(field->message_type()),
            "name", UnderscoresToCapitalizedCamelCase(field));
          break;
      }
    }
  }

  if (descriptor_->extension_range_count() > 0) {
    printer->Print(
      "if (!extensionsAreInitialized()) return false;\r\n");
  }

  printer->Outdent();
  printer->Print(
    "  return true;\r\n"
    "}\r\n"
    "\r\n");
}

}  // namespace csharp
}  // namespace compiler
}  // namespace protobuf
}  // namespace google