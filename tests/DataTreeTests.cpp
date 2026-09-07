// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "DataTree.h"

#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {
template <typename F> void requireTypeMismatch(F operation) {
    bool threw = false;
    try { operation(); } catch (const DataTypeMismatchException& e) {
        threw = true;
        CUBIC_REQUIRE(std::string(e.what()).find("mismatch") != std::string::npos);
    }
    CUBIC_REQUIRE(threw);
}

template <typename T> void exerciseNumericStorage() {
    DataNode node("value");
    node = static_cast<T>(42);
    auto* e = node.element();
    CUBIC_REQUIRE(e->getChar() == 42);
    CUBIC_REQUIRE(e->getUChar() == 42);
    CUBIC_REQUIRE(e->getInt() == 42);
    CUBIC_REQUIRE(e->getUInt() == 42);
    CUBIC_REQUIRE(e->getLong() == 42);
    CUBIC_REQUIRE(e->getULong() == 42);
    CUBIC_REQUIRE(e->getLongLong() == 42);
    CUBIC_REQUIRE(e->getFloat() == 42);
    CUBIC_REQUIRE(e->getDouble() == 42);
    CUBIC_REQUIRE(static_cast<T>(node) == 42);
    CUBIC_REQUIRE(e->getDataSize() == sizeof(T));
    DataElement clone(*e);
    e->set(7);
    CUBIC_REQUIRE(clone.getInt() == 42);

    std::vector<T> values = {T(1), T(42), T(100)};
    e->set(values);
    std::vector<double> converted;
    e->get(converted);
    CUBIC_REQUIRE(converted == (std::vector<double>{1, 42, 100}));
    values.clear();
    e->set(values);
    e->get(converted);
    CUBIC_REQUIRE(converted.empty());
}

template <typename T> void exerciseVectorNode() {
    DataNode node;
    std::vector<T> values = {T(1), T(2)};
    node = values;
    CUBIC_REQUIRE(static_cast<std::vector<T>>(node) == values);
}
}

CUBIC_TEST(data_element_converts_every_numeric_storage_type) {
    exerciseNumericStorage<char>();
    exerciseNumericStorage<unsigned char>();
    exerciseNumericStorage<int>();
    exerciseNumericStorage<unsigned int>();
    exerciseNumericStorage<long>();
    exerciseNumericStorage<unsigned long>();
    exerciseNumericStorage<long long>();
    exerciseNumericStorage<float>();
    exerciseNumericStorage<double>();
    exerciseVectorNode<char>();
    exerciseVectorNode<int>();
    exerciseVectorNode<unsigned int>();
    exerciseVectorNode<long>();
    exerciseVectorNode<unsigned long>();
    exerciseVectorNode<float>();
    exerciseVectorNode<double>();
}

CUBIC_TEST(data_element_empty_strings_binary_and_type_errors) {
    DataElement e;
    CUBIC_REQUIRE(e.getDataPointer() == nullptr);
    CUBIC_REQUIRE(e.getDataSize() == 0);
    CUBIC_REQUIRE(e.toString().empty());
    std::string s = "old";
    std::wstring w = L"old";
    e.get(s); e.get(w);
    CUBIC_REQUIRE(s.empty() && w.empty());
    e.set(std::wstring(L"Radio"));
    e.get(w);
    CUBIC_REQUIRE(w == L"Radio");
    e.set(std::wstring()); e.get(w);
    CUBIC_REQUIRE(w.empty());
    e.set(std::string("station"));
    CUBIC_REQUIRE(e.toString() == "station");
    requireTypeMismatch([&] { e.get(w); });
    requireTypeMismatch([&] { std::set<std::string> v; e.get(v); });
    requireTypeMismatch([&] { DataElement::DataElementBuffer v; e.get(v); });
    e.set(42);
    CUBIC_REQUIRE(e.toString() == "42");
    requireTypeMismatch([&] { e.get(s); });
    e.set(1.5f); CUBIC_REQUIRE(std::stod(e.toString()) == 1.5);
    e.set(2.5); CUBIC_REQUIRE(std::stod(e.toString()) == 2.5);
    e.set(42L); CUBIC_REQUIRE(e.toString() == "42");
    e.set(42LL); CUBIC_REQUIRE(e.toString() == "42");
    DataException error("reason");
    CUBIC_REQUIRE(static_cast<std::string>(error) == "reason");
    DataElement empty;
    bool emptyRejected = false;
    try { empty.getInt(); } catch (const DataException& ex) {
        emptyRejected = true;
        CUBIC_REQUIRE(std::string(ex.what()).find("empty") != std::string::npos);
    }
    CUBIC_REQUIRE(emptyRejected);
}

CUBIC_TEST(data_node_named_iteration_bounds_and_ownership) {
    DataNode root;
    root.setName("root");
    auto* first = root ^ "item";
    *first = std::string("one");
    auto* second = root.newChild("item", new DataNode("item"));
    *second = std::string("two");
    CUBIC_REQUIRE(first->getParentNode() == &root);
    CUBIC_REQUIRE(root.numChildren() == 2);
    CUBIC_REQUIRE(root("item"));
    CUBIC_REQUIRE(root["item"] == first);
    CUBIC_REQUIRE(root["item"] == second);
    CUBIC_REQUIRE(!root("item"));
    root.rewind("item");
    CUBIC_REQUIRE(root.getNext("item") == first);
    CUBIC_REQUIRE(root[1] == second);
    CUBIC_REQUIRE(root());
    CUBIC_REQUIRE(static_cast<std::string>(*first) == "one");
    CUBIC_REQUIRE(static_cast<const char*>(*first) != nullptr);
    *second = 7;
    CUBIC_REQUIRE(static_cast<const char*>(*second) == nullptr);
    for (int index : {-1, 2}) {
        bool named = false, numbered = false;
        try { root.child("item", index); } catch (const DataInvalidChildException&) { named = true; }
        try { root.child(index); } catch (const DataInvalidChildException&) { numbered = true; }
        CUBIC_REQUIRE(named && numbered);
    }
    std::vector<DataNode*> found;
    root.rewindAll();
    root.findAll("root", found);
    CUBIC_REQUIRE(found.size() == 1 && found[0] == &root);
    CUBIC_REQUIRE(root.numChildren("absent") == 0);
}

CUBIC_TEST(data_tree_decodes_numeric_policies_and_string_lists) {
    DataTree tree;
    DataNode value;
    for (auto policy : {USE_FLOAT, USE_DOUBLE}) {
        tree.decodeXMLText(&value, "1.25e+2", policy);
        CUBIC_REQUIRE(value.element()->getDouble() == 125.0);
        tree.decodeXMLText(&value, "1.5 -2.25 3.75", policy);
        CUBIC_REQUIRE(static_cast<std::vector<double>>(value) == (std::vector<double>{1.5, -2.25, 3.75}));
    }
    for (const auto& item : std::vector<std::pair<std::string, std::vector<long long>>>{
        {"1 -2 3", {1, -2, 3}}, {"1000 -2000", {1000, -2000}},
        {"5000000000 -6000000000", {5000000000LL, -6000000000LL}}}) {
        tree.decodeXMLText(&value, item.first.c_str(), USE_DOUBLE);
        std::vector<long long> actual;
        value.element()->get(actual);
        CUBIC_REQUIRE(actual == item.second);
    }
    TiXmlDocument document;
    document.Parse("<?xml version='1.0'?><session><!--comment--><labels><str/><str>FM</str><skip/><str><nested/></str></labels><empty/></session>");
    CUBIC_REQUIRE(!document.Error());
    tree.setFromXML(tree.rootNode(), &document);
    auto* root = tree.rootNode()->child("session");
    std::vector<std::string> labels;
    root->child("labels")->element()->get(labels);
    CUBIC_REQUIRE(labels == (std::vector<std::string>{"", "FM"}));
    CUBIC_REQUIRE(static_cast<std::string>(*root->child("empty")).empty());
}

CUBIC_TEST(data_tree_serializes_numeric_vectors_and_wide_attributes) {
    DataTree tree("session");
    auto* root = tree.rootNode();
    root->newChild("null");
    root->newChild("")->element()->set("unnamed");
    root->newChild("c")->element()->set('Q');
    root->newChild("uc")->element()->set(static_cast<unsigned char>('R'));
    root->newChild("i")->element()->set(123);
    root->newChild("ui")->element()->set(123U);
    root->newChild("l")->element()->set(5000000000L);
    root->newChild("ul")->element()->set(5000000000UL);
    root->newChild("ll")->element()->set(5000000000LL);
    root->newChild("f")->element()->set(1.25f);
    root->newChild("d")->element()->set(2.5);
    root->newChild("cv")->element()->set(std::vector<char>{'A', 'B'});
    root->newChild("iv")->element()->set(std::vector<int>{1000, -2000});
    root->newChild("uiv")->element()->set(std::vector<unsigned int>{1000, 2000});
    root->newChild("lv")->element()->set(std::vector<long>{5000000000L, -6000000000L});
    root->newChild("ulv")->element()->set(std::vector<unsigned long>{5000000000UL, 6000000000UL});
    root->newChild("llv")->element()->set(std::vector<long long>{5000000000LL, -6000000000LL});
    root->newChild("fv")->element()->set(std::vector<float>{1.25f, 2.5f});
    root->newChild("dv")->element()->set(std::vector<double>{1.25, 2.5});
    *root->newChild("wide") = std::wstring(L"Radio");
    *root->newChild("@wide") = std::wstring(L"Name");
    root->newChild("binary")->element()->set("bytes", 5);
    root->newChild("@binary")->element()->set("attr", 4);
    root->newChild("nested")->newChild("child")->element()->set("value");
    TiXmlElement xml("session");
    tree.nodeToXML(root, &xml);
    CUBIC_REQUIRE(std::string(xml.FirstChildElement("c")->GetText()) == "Q");
    CUBIC_REQUIRE(std::string(xml.FirstChildElement("uc")->GetText()) == "R");
    CUBIC_REQUIRE(std::string(xml.FirstChildElement("cv")->GetText()) == "A B");
    CUBIC_REQUIRE(std::string(xml.Attribute("binary")) == "attr");
    DataTree loaded;
    loaded.setFromXML(loaded.rootNode(), &xml);
    std::wstring wide;
    loaded.rootNode()->child("wide")->element()->get(wide);
    CUBIC_REQUIRE(wide == L"Radio");
    loaded.rootNode()->child("@wide")->element()->get(wide);
    CUBIC_REQUIRE(wide == L"Name");
    CUBIC_REQUIRE(static_cast<std::vector<double>>(*loaded.rootNode()->child("fv")) == (std::vector<double>{1.25, 2.5}));
    CUBIC_REQUIRE(static_cast<long long>(*loaded.rootNode()->child("l")) == 5000000000LL);
    CUBIC_REQUIRE(static_cast<std::string>(*loaded.rootNode()->child("nested")->child("child")) == "value");
}

CUBIC_TEST(data_element_round_trips_scalars_and_collections) {
    DataElement element;

    element.set(-123456789LL);
    CUBIC_REQUIRE(element.getLongLong() == -123456789LL);
    element.set(3.25);
    CUBIC_REQUIRE_NEAR(element.getDouble(), 3.25, 0.000001);

    const std::vector<unsigned int> numbers = {1U, 4000000000U};
    element.set(numbers);
    std::vector<unsigned int> numbersOut;
    element.get(numbersOut);
    CUBIC_REQUIRE(numbersOut == numbers);

    const std::vector<std::string> words = {"alpha", "", "gamma"};
    element.set(words);
    std::vector<std::string> wordsOut;
    element.get(wordsOut);
    CUBIC_REQUIRE(wordsOut == words);

    const std::set<std::string> tags = {"am", "fm", "ssb"};
    element.set(tags);
    std::set<std::string> tagsOut;
    element.get(tagsOut);
    CUBIC_REQUIRE(tagsOut == tags);
}

CUBIC_TEST(data_element_preserves_binary_payloads_and_rejects_wrong_getters) {
    DataElement element;
    const char payload[] = {'a', '\0', 'b', '\x7f'};
    element.set(payload, sizeof(payload));

    DataElement::DataElementBuffer output;
    element.get(output);
    CUBIC_REQUIRE(output.size() == sizeof(payload));
    CUBIC_REQUIRE(output[1] == 0);
    CUBIC_REQUIRE(output[3] == 0x7f);

    bool threw = false;
    try {
        std::vector<std::string> invalid;
        element.get(invalid);
    } catch (const DataTypeMismatchException&) {
        threw = true;
    }
    CUBIC_REQUIRE(threw);
}

CUBIC_TEST(data_node_supports_hierarchy_iteration_search_and_cloning) {
    DataNode root("root");
    root.newChild("radio")->newChild("mode")->element()->set("FM");
    root.newChild("radio")->newChild("mode")->element()->set("AM");
    CUBIC_REQUIRE(root.numChildren("radio") == 2);

    std::vector<DataNode*> modes;
    root.findAll("mode", modes);
    CUBIC_REQUIRE(modes.size() == 2);

    DataNode clone("copy", root);
    CUBIC_REQUIRE(clone.numChildren("radio") == 2);
    CUBIC_REQUIRE(root.hasAnother());
    root.rewindAll();

    bool threw = false;
    try {
        root.child("missing");
    } catch (const DataInvalidChildException&) {
        threw = true;
    }
    CUBIC_REQUIRE(threw);
}

CUBIC_TEST(data_tree_round_trips_xml_values_vectors_and_attributes) {
    DataTree tree("session");
    tree.rootNode()->newChild("@version")->element()->set("2");
    tree.rootNode()->newChild("frequency")->element()->set(101700000LL);
    tree.rootNode()->newChild("gain")->element()->set(12.5);
    tree.rootNode()->newChild("labels")->element()->set(std::vector<std::string>{"local", "music"});
    tree.rootNode()->newChild("bins")->element()->set(std::vector<unsigned int>{1, 2, 4000000000U});

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path file = std::filesystem::temp_directory_path() /
        ("cubicsdr-datatree-" + std::to_string(unique) + ".xml");

    CUBIC_REQUIRE(tree.SaveToFileXML(file.string()));
    DataTree loaded;
    CUBIC_REQUIRE(loaded.LoadFromFileXML(file.string(), USE_DOUBLE));
    std::filesystem::remove(file);

    CUBIC_REQUIRE(loaded.rootNode()->getName() == "session");
    CUBIC_REQUIRE(static_cast<int>(*loaded.rootNode()->child("@version")) == 2);
    CUBIC_REQUIRE(static_cast<long long>(*loaded.rootNode()->child("frequency")) == 101700000LL);
    CUBIC_REQUIRE_NEAR(static_cast<double>(*loaded.rootNode()->child("gain")), 12.5, 0.000001);
    std::vector<std::string> labels;
    loaded.rootNode()->child("labels")->element()->get(labels);
    CUBIC_REQUIRE(labels == (std::vector<std::string>{"local", "music"}));
    CUBIC_REQUIRE(static_cast<std::vector<unsigned int>>(*loaded.rootNode()->child("bins")) ==
                  (std::vector<unsigned int>{1, 2, 4000000000U}));
}
