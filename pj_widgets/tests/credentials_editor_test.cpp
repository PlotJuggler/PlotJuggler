// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryFile>

#include "pj_widgets/CredentialsEditor.h"
using namespace Qt::StringLiterals;

namespace {

const QString kTick = QString::fromUtf8("✓");   // green check
const QString kCross = QString::fromUtf8("✗");  // red cross
// An example provider-specific pattern + a key that matches it. The widget itself
// is domain-neutral; a plugin injects this pattern via the `apiKeyPattern` property.
const QString kKeyPattern = u"^msco_[a-z0-9]{32}_[0-9a-f]{8}$"_s;
const QString kValidKey = u"msco_abcdefghijklmnopqrstuvwxyz012345_0123abcd"_s;

// The sub-dialog harvester (PanelEngine) walks findChildren by objectName and
// drives the plugin's onTextChanged/onToggled from these exact names, so they
// are the load-bearing contract of this widget.
TEST(CredentialsEditor, ExposesNamedChildInputsForHarvester) {
  PJ::CredentialsEditor editor;

  EXPECT_NE(editor.findChild<QLineEdit*>("certPath"), nullptr);
  EXPECT_NE(editor.findChild<QLineEdit*>("apiKey"), nullptr);
  EXPECT_NE(editor.findChild<QCheckBox*>("allowInsecure"), nullptr);
}

TEST(CredentialsEditor, ApiKeyMaskedByDefaultAndRevealToggleShowsIt) {
  PJ::CredentialsEditor editor;

  auto* api_key = editor.findChild<QLineEdit*>("apiKey");
  ASSERT_NE(api_key, nullptr);
  EXPECT_EQ(api_key->echoMode(), QLineEdit::Password);

  auto* reveal = editor.findChild<QAbstractButton*>("apiKeyReveal");
  ASSERT_NE(reveal, nullptr);
  ASSERT_TRUE(reveal->isCheckable());

  reveal->setChecked(true);
  EXPECT_EQ(api_key->echoMode(), QLineEdit::Normal);

  reveal->setChecked(false);
  EXPECT_EQ(api_key->echoMode(), QLineEdit::Password);
}

// Validity predicate is pure, so tested directly. With no pattern the widget is
// domain-neutral: any non-empty key is valid.
TEST(CredentialsEditor, IsApiKeyValidDefaultsToNonEmpty) {
  EXPECT_TRUE(PJ::CredentialsEditor::isApiKeyValid(u"anything-non-empty"_s));
  EXPECT_TRUE(PJ::CredentialsEditor::isApiKeyValid(kValidKey));
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(QString()));
}

// An injected (plugin-owned) pattern is enforced as a full match.
TEST(CredentialsEditor, IsApiKeyValidEnforcesInjectedPattern) {
  EXPECT_TRUE(PJ::CredentialsEditor::isApiKeyValid(kValidKey, kKeyPattern));
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(QString(), kKeyPattern));
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(u"msco_short_0123abcd"_s, kKeyPattern));
  // Hex suffix must be lowercase hex, not arbitrary alnum.
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(u"msco_abcdefghijklmnopqrstuvwxyz012345_0123ABCD"_s, kKeyPattern));
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(u"nope_abcdefghijklmnopqrstuvwxyz012345_0123abcd"_s, kKeyPattern));
  // A partial match (valid prefix, trailing junk) must be rejected.
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(kValidKey + u"trailing"_s, kKeyPattern));
}

// A malformed pattern degrades to the non-empty rule rather than rejecting all.
TEST(CredentialsEditor, IsApiKeyValidFallsBackOnBadPattern) {
  const QString bad = u"([unterminated"_s;
  EXPECT_TRUE(PJ::CredentialsEditor::isApiKeyValid(u"key"_s, bad));
  EXPECT_FALSE(PJ::CredentialsEditor::isApiKeyValid(QString(), bad));
}

TEST(CredentialsEditor, IsCertReadableChecksFileExistence) {
  EXPECT_FALSE(PJ::CredentialsEditor::isCertReadable(QString()));
  EXPECT_FALSE(PJ::CredentialsEditor::isCertReadable(u"/no/such/ca.pem"_s));
  QTemporaryFile f;
  ASSERT_TRUE(f.open());
  EXPECT_TRUE(PJ::CredentialsEditor::isCertReadable(f.fileName()));
}

// Live ticks: PJ3 CertDialog showed ✓/✗ as the user typed (cert_dialog.cpp:139-177).
// Pre-fill via setText fires textChanged too, so initial validity also shows.
// Default (no injected pattern) → any non-empty key is valid.
TEST(CredentialsEditor, ApiKeyTickDefaultsToNonEmptyLive) {
  PJ::CredentialsEditor editor;
  auto* api_key = editor.findChild<QLineEdit*>("apiKey");
  auto* tick = editor.findChild<QLabel*>("apiKeyTick");
  ASSERT_NE(api_key, nullptr);
  ASSERT_NE(tick, nullptr);

  EXPECT_TRUE(tick->text().isEmpty());  // empty key → no tick
  api_key->setText(u"any-non-empty"_s);
  EXPECT_EQ(tick->text(), kTick);
  api_key->clear();
  EXPECT_TRUE(tick->text().isEmpty());
}

// With a plugin-injected pattern the tick enforces it, and setApiKeyPattern
// re-evaluates immediately against the current text.
TEST(CredentialsEditor, ApiKeyTickEnforcesInjectedPatternLive) {
  PJ::CredentialsEditor editor;
  auto* api_key = editor.findChild<QLineEdit*>("apiKey");
  auto* tick = editor.findChild<QLabel*>("apiKeyTick");
  ASSERT_NE(api_key, nullptr);
  ASSERT_NE(tick, nullptr);

  editor.setApiKeyPattern(kKeyPattern);
  api_key->setText(kValidKey);
  EXPECT_EQ(tick->text(), kTick);
  api_key->setText(u"garbage"_s);
  EXPECT_EQ(tick->text(), kCross);

  // Setting the pattern after text re-runs the tick (here: relax to non-empty).
  editor.setApiKeyPattern(QString());
  EXPECT_EQ(tick->text(), kTick);
}

TEST(CredentialsEditor, CertTickReflectsReadabilityLive) {
  PJ::CredentialsEditor editor;
  auto* cert = editor.findChild<QLineEdit*>("certPath");
  auto* tick = editor.findChild<QLabel*>("certTick");
  ASSERT_NE(cert, nullptr);
  ASSERT_NE(tick, nullptr);

  EXPECT_TRUE(tick->text().isEmpty());
  cert->setText(u"/no/such/ca.pem"_s);
  EXPECT_EQ(tick->text(), kCross);
  QTemporaryFile f;
  ASSERT_TRUE(f.open());
  cert->setText(f.fileName());
  EXPECT_EQ(tick->text(), kTick);
}

}  // namespace
