/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Tests adding a rule on elements nested in iframes.

const TEST_URI =
 `<div>outer</div>
  <iframe id="frame1" src="data:text/html;charset=utf-8,<div>inner1</div>">
  </iframe>
  <iframe id="frame2" src="data:text/html;charset=utf-8,<div>inner2</div>">
  </iframe>`;

add_task(async function() {
  await addTab("data:text/html;charset=utf-8," + encodeURIComponent(TEST_URI));
  let {inspector, view} = await openRuleView();
  await selectNode("div", inspector);
  await addNewRuleAndDismissEditor(inspector, view, "div", 1);
  await addNewProperty(view, 1, "color", "red");

  let innerFrameDiv1 = await getNodeFrontInFrame("div", "#frame1", inspector);
  await selectNode(innerFrameDiv1, inspector);
  await addNewRuleAndDismissEditor(inspector, view, "div", 1);
  await addNewProperty(view, 1, "color", "blue");

  let innerFrameDiv2 = await getNodeFrontInFrame("div", "#frame2", inspector);
  await selectNode(innerFrameDiv2, inspector);
  await addNewRuleAndDismissEditor(inspector, view, "div", 1);
  await addNewProperty(view, 1, "color", "green");
});

/**
 * Add a new property in the rule at the provided index in the rule view.
 *
 * @param {RuleView} view
 * @param {Number} index
 *        The index of the rule in which we should add a new property.
 * @param {String} name
 *        The name of the new property.
 * @param {String} value
 *        The value of the new property.
 */
async function addNewProperty(view, index, name, value) {
  let idRuleEditor = getRuleViewRuleEditor(view, index);
  info(`Adding new property "${name}: ${value};"`);

  let onRuleViewChanged = view.once("ruleview-changed");
  idRuleEditor.addProperty(name, value, "", true);
  await onRuleViewChanged;

  let textProps = idRuleEditor.rule.textProps;
  let lastProperty = textProps[textProps.length - 1];
  is(lastProperty.name, name, "Last property has the expected name");
  is(lastProperty.value, value, "Last property has the expected value");
}
