// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "DataTree.h"

#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

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
