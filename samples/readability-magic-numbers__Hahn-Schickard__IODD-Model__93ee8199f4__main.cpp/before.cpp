#include "Repository.hpp"

#include "Variant_Visitor.hpp"

#include <iostream>

using namespace std;
using namespace IODD;

constexpr uint8_t PADDING_SIZE = 1;

string pad(size_t times = 1) { return string(times * PADDING_SIZE, ' '); }

string padDot(size_t times = 1) { return pad(times) + "•" + pad(); }

void printVariable(const VariablePtr& var, const size_t offset = 0) {
  cout << pad(offset) << var->index() << " " << toString(var->type()) << " "
       << var->name().locale();
}

void printCondition(const optional<Condition>& condition, const size_t offset) {
  if (condition) {
    cout << pad(offset) << "Is used if variable: ";
    printVariable(condition->variable());
    if (auto subindex = condition->subindex()) {
      cout << " subindex " << (size_t)*subindex;
    }
    cout << " is equal to " << (size_t)condition->value() << endl;
  }
}

void printButton(const VariableRefPtr& ref, const size_t offset) {
  cout << pad(offset) << "That is used as a button";
  if (auto desc = ref->description()) {
    cout << " that " << desc->locale();
  }

  if (auto action_msg = ref->actionMessage()) {
    cout << " that " << action_msg->locale();
  }

  cout << endl << pad(offset) << "This button sends";
  match(
      ref->buttonValue(),
      [](bool value) { cout << " boolean value: " << boolalpha << value; },
      [](uint64_t value) { cout << " unsigned value: " << value; },
      [](int64_t value) { cout << " integer value: " << value; });
  cout << " to the Device" << endl;
}

void printRefRepresentation(const VariableRefPtr& ref, const size_t offset) {
  if (ref->isButton()) {
    printButton(ref, offset);
  } else {
    if (auto unit = ref->unit()) {
      cout << pad(offset) << "It measures";
      if (auto name = unit->name()) {
        cout << " " << name->locale();
      }
      auto symbol = unit->abbr();
      cout << " in " << symbol << endl;
    }
    if (isDecimal(ref->displayFormat())) {
      if (isNumericData(ref->variable()->type()) ||
          isNumericComplexData(ref->variable()->type())) {
        cout << pad(offset)
             << "The measurement is calculated as follows: " << endl;
        cout << pad(offset) << "(Raw Value * " << ref->gradient() << ") + "
             << ref->offset() << endl;
      }
    } else if (isBinary(ref->displayFormat())) {
      cout << pad(offset) << "It is represented as a binary value" << endl;
    } else {
      cout << pad(offset) << "It is represented as a hexadecimal value" << endl;
    }
  }
}

void printVarRef(const VariableRefPtr& ref, const size_t offset) {
  cout << "Variable reference for ";
  printVariable(ref->variable());
  cout << endl;
  printRefRepresentation(ref, offset);
}

void printRecordRef(const RecordRefPtr& ref, const size_t offset) {
  cout << "RecordItem reference for ";
  printVariable(ref->variable());
  cout << " subindex " << (size_t)ref->subindex() << endl;
  printRefRepresentation(ref, offset);
}

void printMenu(const MenuPtr& menu, const size_t offset) {
  cout << pad(offset) << "Menu id: " << menu->id();
  if (auto name = menu->name()) {
    cout << " " << name->locale();
  }
  cout << " contains:" << endl;
  printCondition(menu->condition(), offset + PADDING_SIZE);
  for (const auto& sub_menu : menu->references()) {
    cout << padDot(offset + PADDING_SIZE) << " ";
    match(
        sub_menu,
        [&offset](const VariableRefPtr& variable_ref) {
          printVarRef(variable_ref, offset + PADDING_SIZE);
        },
        [&offset](const RecordRefPtr& record_ref) {
          printRecordRef(record_ref, offset + PADDING_SIZE);
        },
        [&offset](
            const MenuPtr& menu) { printMenu(menu, offset + PADDING_SIZE); });
  }
}

void printUI(const UserInterfacePtr& ui) {
  cout << pad() << toString(ui->role()) << " UI has:" << endl;

  auto ident = ui->identification();
  cout << padDot(2) << ident->size() << " identification menus" << endl;
  printMenu(ident, 2);

  if (auto params = ui->parameter()) {
    cout << padDot(2) << params->size() << " parameter menus" << endl;
    printMenu(params, 2);
  }

  if (auto observ = ui->observation()) {
    cout << padDot(2) << observ->size() << " observation menus" << endl;
    printMenu(observ, 2);
  }

  if (auto diag = ui->diagnosis()) {
    cout << padDot(2) << diag->size() << " diagnosis menus" << endl;
    printMenu(diag, 2);
  }
}

void printDescriptor(const DeviceDescriptorPtr& descriptor) {
  cout << pad() << "Device " << descriptor->getDeviceName().locale()
       << " is manufactured by " << descriptor->getVendorName() << endl;
  cout << pad() << "It has " << descriptor->variableCount()
       << " variables:" << endl;

  for (const auto& var : descriptor->getVariables()) {
    cout << padDot() << "Variable: " << var.first;
    printVariable(var.second, 1);
    cout << endl;
  }

  auto uis = descriptor->getUIs();
  cout << pad() << "It has " << uis.size() << " user interfaces:" << endl;
  for (const auto& ui : uis) {
    printUI(ui.second);
  }
}

int main() {
  try {
    auto repo = makeRepository(filesystem::path("config"));

    cout << "IODD Repository has " << repo->size() << " descriptors" << endl;
    for (const auto& [id, descriptor] : repo->getDescriptors()) {
      cout << "Device descriptor for " << id << " device:" << endl;
      printDescriptor(descriptor);
    }

    vector<uint8_t> bytes = {0x00, 0xCD}; // fake input date
    auto descriptor = repo->getDescriptor("310", "375");
    auto menu = descriptor->getObserverUI()->getMenu("M_MR_SR_Observe_C");

    cout << "Found Device 310-375 Observer UI menu " << menu->id() << endl;

    auto ref = menu->references()[0];
    if (holds_alternative<RecordRefPtr>(ref)) {
      auto record_ref = get<RecordRefPtr>(ref);
      auto value = record_ref->decode(bytes);
      cout << "Device 310-375 menu " << menu->id()
           << " value: " << toString(value) << endl;
    }

    exit(EXIT_SUCCESS);
  } catch (const exception& ex) {
    cerr << "Encountered an exception: " << ex.what() << endl;
    exit(EXIT_FAILURE);
  }
}