/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ScreenSettingsDialog.h"
#include "ui_ScreenSettingsDialog.h"

#include "gui/config/Screen.h"
#include "validators/AliasValidator.h"
#include "validators/ScreenNameValidator.h"
#include "validators/ValidationError.h"

#include <QMessageBox>

using enum ScreenConfig::Modifier;
using enum ScreenConfig::SwitchCorner;
using enum ScreenConfig::Fix;

ScreenSettingsDialog::~ScreenSettingsDialog() = default;

ScreenSettingsDialog::ScreenSettingsDialog(QWidget *parent, Screen *screen, const ScreenList *screens)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ScreenSettingsDialog>()},
      m_screen(screen)
{

  ui->setupUi(this);
  ui->buttonBox->button(QDialogButtonBox::Cancel)->setFocus();

  ui->lineNameEdit->setText(m_screen->name());

  const auto valNameError = new validators::ValidationError(this, ui->lblNameError);
  const auto valName = new validators::ScreenNameValidator(ui->lineNameEdit, valNameError, screens);
  ui->lineNameEdit->setValidator(valName);

  const auto valAliasError = new validators::ValidationError(this, ui->lblAliasError);
  const auto valAlias = new validators::AliasValidator(ui->lineAddAlias, valAliasError);
  ui->lineAddAlias->setValidator(valAlias);

  for (int i = 0; i < m_screen->aliases().count(); i++)
    new QListWidgetItem(m_screen->aliases()[i], ui->listAliases);

  ui->comboShift->setCurrentIndex(m_screen->modifier(static_cast<int>(Shift)));
  ui->comboCtrl->setCurrentIndex(m_screen->modifier(static_cast<int>(Ctrl)));
  ui->comboAlt->setCurrentIndex(m_screen->modifier(static_cast<int>(Alt)));
  ui->comboMeta->setCurrentIndex(m_screen->modifier(static_cast<int>(Meta)));
  ui->comboSuper->setCurrentIndex(m_screen->modifier(static_cast<int>(Super)));

  // Detect which preset matches current settings and select it
  ui->comboPreset->setCurrentIndex(detectCurrentPreset());

  connect(ui->comboPreset, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &ScreenSettingsDialog::applyPreset);

  ui->chkDeadTopLeft->setChecked(m_screen->switchCorner(static_cast<int>(TopLeft)));
  ui->chkDeadTopRight->setChecked(m_screen->switchCorner(static_cast<int>(TopRight)));
  ui->chkDeadBottomLeft->setChecked(m_screen->switchCorner(static_cast<int>(BottomLeft)));
  ui->chkDeadBottomRight->setChecked(m_screen->switchCorner(static_cast<int>(BottomRight)));
  ui->sbSwitchCornerSize->setValue(m_screen->switchCornerSize());

  ui->chkFixCapsLock->setChecked(m_screen->fix(CapsLock));
  ui->chkFixNumLock->setChecked(m_screen->fix(NumLock));
  ui->chkFixScrollLock->setChecked(m_screen->fix(ScrollLock));
  ui->chkFixXTest->setChecked(m_screen->fix(XTest));

  connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &ScreenSettingsDialog::accept);
  connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &ScreenSettingsDialog::reject);
  connect(ui->btnAddAlias, &QPushButton::clicked, this, &ScreenSettingsDialog::addAlias);
  connect(ui->btnRemoveAlias, &QPushButton::clicked, this, &ScreenSettingsDialog::removeAlias);
  connect(ui->lineAddAlias, &QLineEdit::textChanged, this, &ScreenSettingsDialog::checkNewAliasName);
  connect(ui->listAliases, &QListWidget::itemSelectionChanged, this, &ScreenSettingsDialog::aliasSelected);
}

void ScreenSettingsDialog::accept()
{
  if (ui->lineNameEdit->text().isEmpty()) {
    QMessageBox::warning(
        this, tr("Screen name is empty"),
        tr("The screen name cannot be empty. "
           "Please either fill in a name or cancel the dialog.")
    );
    return;
  }
  if (!ui->lblNameError->text().isEmpty()) {
    return;
  }

  m_screen->setName(ui->lineNameEdit->text());

  for (int i = 0; i < ui->listAliases->count(); i++) {
    QString alias(ui->listAliases->item(i)->text());
    if (alias == ui->lineNameEdit->text()) {
      QMessageBox::warning(
          this, tr("Screen name matches alias"),
          tr("The screen name cannot be the same as an alias. "
             "Please either remove the alias or change the screen name.")
      );
      return;
    }
    m_screen->addAlias(alias);
  }

  m_screen->setModifier(Shift, ui->comboShift->currentIndex());
  m_screen->setModifier(Ctrl, ui->comboCtrl->currentIndex());
  m_screen->setModifier(Alt, ui->comboAlt->currentIndex());
  m_screen->setModifier(Meta, ui->comboMeta->currentIndex());
  m_screen->setModifier(Super, ui->comboSuper->currentIndex());

  m_screen->setSwitchCorner(TopLeft, ui->chkDeadTopLeft->isChecked());
  m_screen->setSwitchCorner(TopRight, ui->chkDeadTopRight->isChecked());
  m_screen->setSwitchCorner(BottomLeft, ui->chkDeadBottomLeft->isChecked());
  m_screen->setSwitchCorner(BottomRight, ui->chkDeadBottomRight->isChecked());
  m_screen->setSwitchCornerSize(ui->sbSwitchCornerSize->value());

  m_screen->setFix(CapsLock, ui->chkFixCapsLock->isChecked());
  m_screen->setFix(NumLock, ui->chkFixNumLock->isChecked());
  m_screen->setFix(ScrollLock, ui->chkFixScrollLock->isChecked());
  m_screen->setFix(XTest, ui->chkFixXTest->isChecked());

  QDialog::accept();
}

void ScreenSettingsDialog::addAlias()
{
  if (!ui->lineAddAlias->text().isEmpty() &&
      ui->listAliases->findItems(ui->lineAddAlias->text(), Qt::MatchFixedString).isEmpty()) {
    new QListWidgetItem(ui->lineAddAlias->text(), ui->listAliases);
    ui->lineAddAlias->clear();
  }
}

void ScreenSettingsDialog::removeAlias() const
{
  QList<QListWidgetItem *> items = ui->listAliases->selectedItems();
  qDeleteAll(items);
}

void ScreenSettingsDialog::checkNewAliasName(const QString &text)
{
  ui->btnAddAlias->setEnabled(!text.isEmpty() && ui->lblAliasError->text().isEmpty());
}

void ScreenSettingsDialog::aliasSelected()
{
  ui->btnRemoveAlias->setEnabled(!ui->listAliases->selectedItems().isEmpty());
}

int ScreenSettingsDialog::detectCurrentPreset() const
{
  int shift = ui->comboShift->currentIndex();
  int ctrl = ui->comboCtrl->currentIndex();
  int alt = ui->comboAlt->currentIndex();
  int meta = ui->comboMeta->currentIndex();
  int super = ui->comboSuper->currentIndex();

  // Check if matches "Default" preset
  if (shift == static_cast<int>(Shift) &&
      ctrl == static_cast<int>(Ctrl) &&
      alt == static_cast<int>(Alt) &&
      meta == static_cast<int>(Meta) &&
      super == static_cast<int>(Super)) {
    return 0; // Default
  }

  // Check if matches "Windows -> Mac" preset
  if (shift == static_cast<int>(Shift) &&
      ctrl == static_cast<int>(Super) &&
      alt == static_cast<int>(Ctrl) &&
      meta == static_cast<int>(Alt) &&
      super == static_cast<int>(Alt)) {
    return 1; // Windows -> Mac
  }

  // Check if matches "Mac -> Windows" preset
  if (shift == static_cast<int>(Shift) &&
      ctrl == static_cast<int>(Alt) &&
      alt == static_cast<int>(Super) &&
      meta == static_cast<int>(Meta) &&
      super == static_cast<int>(Ctrl)) {
    return 2; // Mac -> Windows
  }

  // No preset matches
  return 0;
}

void ScreenSettingsDialog::applyPreset(int presetIndex)
{
  // Preset index: 0 = Default, 1 = Windows->Mac, 2 = Mac->Windows
  switch (presetIndex) {
  case 0: // Default - no remapping
    ui->comboShift->setCurrentIndex(static_cast<int>(Shift));
    ui->comboCtrl->setCurrentIndex(static_cast<int>(Ctrl));
    ui->comboAlt->setCurrentIndex(static_cast<int>(Alt));
    ui->comboMeta->setCurrentIndex(static_cast<int>(Meta));
    ui->comboSuper->setCurrentIndex(static_cast<int>(Super));
    break;

  case 1: // Windows -> Mac (Windows client connecting to Mac server)
    // Windows Ctrl -> Mac Command (Super)
    // Windows Alt -> Mac Control (Ctrl)
    // Windows Win (Meta) -> Mac Option (Alt)
    // Windows Win (Super) -> Mac Option (Alt)
    ui->comboShift->setCurrentIndex(static_cast<int>(Shift));
    ui->comboCtrl->setCurrentIndex(static_cast<int>(Super));
    ui->comboAlt->setCurrentIndex(static_cast<int>(Ctrl));
    ui->comboMeta->setCurrentIndex(static_cast<int>(Alt));
    ui->comboSuper->setCurrentIndex(static_cast<int>(Alt));
    break;

  case 2: // Mac -> Windows (Mac client connecting to Windows server)
    // Mac Command (Super) -> Windows Ctrl
    // Mac Control (Ctrl) -> Windows Alt
    // Mac Option (Alt) -> Windows Win (Meta/Super)
    ui->comboShift->setCurrentIndex(static_cast<int>(Shift));
    ui->comboCtrl->setCurrentIndex(static_cast<int>(Alt));
    ui->comboAlt->setCurrentIndex(static_cast<int>(Super));
    ui->comboMeta->setCurrentIndex(static_cast<int>(Meta));
    ui->comboSuper->setCurrentIndex(static_cast<int>(Ctrl));
    break;
  }
}
