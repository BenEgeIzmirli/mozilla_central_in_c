/* vim: set ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

// Test that attribute items work in the context menu

const TEST_URL = URL_ROOT + "doc_inspector_menu.html";

add_task(async function() {
  let { inspector, testActor } = await openInspectorForURL(TEST_URL);
  await selectNode("#attributes", inspector);

  await testAddAttribute();
  await testCopyAttributeValue();
  await testCopyLongAttributeValue();
  await testEditAttribute();
  await testRemoveAttribute();

  async function testAddAttribute() {
    info("Triggering 'Add Attribute' and waiting for mutation to occur");
    let addAttribute = getMenuItem("node-menu-add-attribute");
    addAttribute.click();

    EventUtils.sendString('class="u-hidden"');
    let onMutation = inspector.once("markupmutation");
    EventUtils.synthesizeKey("KEY_Enter");
    await onMutation;

    let hasAttribute = testActor.hasNode("#attributes.u-hidden");
    ok(hasAttribute, "attribute was successfully added");
  }

  async function testCopyAttributeValue() {
    info("Testing 'Copy Attribute Value' and waiting for clipboard promise to resolve");
    let copyAttributeValue = getMenuItem("node-menu-copy-attribute");

    info("Triggering 'Copy Attribute Value' and waiting for clipboard to copy the value");
    inspector.nodeMenuTriggerInfo = {
      type: "attribute",
      name: "data-edit",
      value: "the"
    };

    await waitForClipboardPromise(() => copyAttributeValue.click(), "the");
  }

  async function testCopyLongAttributeValue() {
    info("Testing 'Copy Attribute Value' copies very long attribute values");
    let copyAttributeValue = getMenuItem("node-menu-copy-attribute");
    let longAttribute = "#01234567890123456789012345678901234567890123456789" +
    "12345678901234567890123456789012345678901234567890123456789012345678901" +
    "23456789012345678901234567890123456789012345678901234567890123456789012" +
    "34567890123456789012345678901234567890123456789012345678901234567890123";

    inspector.nodeMenuTriggerInfo = {
      type: "attribute",
      name: "data-edit",
      value: longAttribute
    };

    await waitForClipboardPromise(() => copyAttributeValue.click(), longAttribute);
  }

  async function testEditAttribute() {
    info("Testing 'Edit Attribute' menu item");
    let editAttribute = getMenuItem("node-menu-edit-attribute");

    info("Triggering 'Edit Attribute' and waiting for mutation to occur");
    inspector.nodeMenuTriggerInfo = {
      type: "attribute",
      name: "data-edit"
    };
    editAttribute.click();
    EventUtils.sendString("data-edit='edited'");
    let onMutation = inspector.once("markupmutation");
    EventUtils.synthesizeKey("KEY_Enter");
    await onMutation;

    let isAttributeChanged =
      await testActor.hasNode("#attributes[data-edit='edited']");
    ok(isAttributeChanged, "attribute was successfully edited");
  }

  async function testRemoveAttribute() {
    info("Testing 'Remove Attribute' menu item");
    let removeAttribute = getMenuItem("node-menu-remove-attribute");

    info("Triggering 'Remove Attribute' and waiting for mutation to occur");
    inspector.nodeMenuTriggerInfo = {
      type: "attribute",
      name: "data-remove"
    };
    let onMutation = inspector.once("markupmutation");
    removeAttribute.click();
    await onMutation;

    let hasAttribute = await testActor.hasNode("#attributes[data-remove]");
    ok(!hasAttribute, "attribute was successfully removed");
  }

  function getMenuItem(id) {
    let allMenuItems = openContextMenuAndGetAllItems(inspector, {
      target: getContainerForSelector("#attributes", inspector).tagLine,
    });
    let menuItem = allMenuItems.find(i => i.id === id);
    ok(menuItem, "Menu item '" + id + "' found");
    // Close the menu so synthesizing future keys won't select menu items.
    EventUtils.synthesizeKey("KEY_Escape");
    return menuItem;
  }
});