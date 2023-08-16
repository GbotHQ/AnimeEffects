#ifndef EXPORTDIAG_H
#define EXPORTDIAG_H
//
// Created by Yukusai on 16 Aug 2023.
//
    /********************************************************************************
** Form generated from reading UI file 'exportRedesignAxBvyn.ui'
**
** Created by: Qt User Interface Compiler version 5.15.5
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

    #include <QtCore/QVariant>
    #include <QtGui/QIcon>
    #include <QtWidgets/QApplication>
    #include <QtWidgets/QCheckBox>
    #include <QtWidgets/QComboBox>
    #include <QtWidgets/QFormLayout>
    #include <QtWidgets/QFrame>
    #include <QtWidgets/QGridLayout>
    #include <QtWidgets/QHBoxLayout>
    #include <QtWidgets/QLabel>
    #include <QtWidgets/QLineEdit>
    #include <QtWidgets/QPushButton>
    #include <QtWidgets/QRadioButton>
    #include <QtWidgets/QScrollArea>
    #include <QtWidgets/QSpinBox>
    #include <QtWidgets/QTabWidget>
    #include <QtWidgets/QTextEdit>
    #include <QtWidgets/QToolButton>
    #include <QtWidgets/QVBoxLayout>
    #include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class ExportWidgetUI {
public:
    QVBoxLayout *verticalLayout_2;
    QVBoxLayout *tabVBox;
    QTabWidget *settingsTab;
    QWidget *globalParam;
    QVBoxLayout *verticalLayout_3;
    QScrollArea *globalParamScroll;
    QWidget *globalParamScrollContents;
    QGridLayout *gridLayout_2;
    QCheckBox *intermediateParamCheckBox;
    QRadioButton *keepAspectRatio;
    QLabel *lastFrameLabel;
    QLabel *exportTypeLabel;
    QLabel *intermediateTypeLabel;
    QRadioButton *customRatio;
    QPushButton *insertFrameRangeButton;
    QFrame *exportLine1;
    QCheckBox *customPaletteCheckBox;
    QSpinBox *firstFrameSpinBox;
    QSpinBox *heightSpinBox;
    QCheckBox *forePipeCheckBox;
    QComboBox *intermediateTypeCombo;
    QLabel *widthLabel;
    QRadioButton *oneToOneRatio;
    QPushButton *setWidthNative;
    QFrame *renderLine1;
    QFrame *exportLine2;
    QCheckBox *postParamCheckBox;
    QLabel *paramsLabel;
    QSpinBox *widthSpinBox;
    QLabel *heightLabel;
    QLabel *exportingLabel;
    QPushButton *setHeightNative;
    QLabel *fpsLabel;
    QLabel *renderingLabel;
    QComboBox *exportTypeCombo;
    QCheckBox *transparencyCheckBox;
    QSpinBox *lastFrameSpinBox;
    QLineEdit *bitrateLineEdit;
    QLabel *renderedFramesLabel;
    QFrame *renderLine2;
    QLabel *firstFrameLabel;
    QLabel *bitrateLabel;
    QCheckBox *allowParamsCheckBox;
    QSpinBox *fpsCombo;
    QToolButton *selectPalettePathButton;
    QWidget *formatParam;
    QVBoxLayout *verticalLayout_4;
    QFormLayout *formatLayout;
    QScrollArea *formatParamScroll;
    QWidget *formatParamScrollContents;
    QGridLayout *gridLayout;
    QLabel *pixelFormatLabel;
    QLabel *movLabel;
    QComboBox *aviCombo;
    QLabel *mkvLabel;
    QComboBox *pixelFormatCombo;
    QComboBox *movCombo;
    QFrame *formatLine;
    QLabel *mp4Label;
    QLabel *aviLabel;
    QComboBox *webmCombo;
    QComboBox *mp4Combo;
    QLabel *webmLabel;
    QComboBox *mkvCombo;
    QWidget *customParam;
    QVBoxLayout *verticalLayout_5;
    QGridLayout *customParamLayout;
    QLabel *intermediateLabel;
    QLabel *postLabel;
    QFrame *customLine;
    QPushButton *addPresetToIntermediate;
    QPushButton *AddPressetToPost;
    QLabel *presetLabel;
    QComboBox *presetCombo;
    QPushButton *removePreset;
    QTextEdit *postParamTextEdit;
    QToolButton *savePostParamAsPreset;
    QTextEdit *intermediateParamTextEdit;
    QToolButton *saveIntermediateParamAsPreset;
    QLabel *commandsLabel;
    QHBoxLayout *exportLayout;
    QPushButton *exportButton;
    QPushButton *cancelButton;

    void setupUi(QWidget *exportWidget)
    {
        if (exportWidget->objectName().isEmpty())
            exportWidget->setObjectName(QString::fromUtf8("exportWidget"));
        exportWidget->resize(525, 675);
        QSizePolicy sizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(exportWidget->sizePolicy().hasHeightForWidth());
        exportWidget->setSizePolicy(sizePolicy);
        verticalLayout_2 = new QVBoxLayout(exportWidget);
        verticalLayout_2->setObjectName(QString::fromUtf8("verticalLayout_2"));
        tabVBox = new QVBoxLayout();
        tabVBox->setSpacing(7);
        tabVBox->setObjectName(QString::fromUtf8("tabVBox"));
        settingsTab = new QTabWidget(exportWidget);
        settingsTab->setObjectName(QString::fromUtf8("settingsTab"));
        settingsTab->setTabShape(QTabWidget::Triangular);
        globalParam = new QWidget();
        globalParam->setObjectName(QString::fromUtf8("globalParam"));
        verticalLayout_3 = new QVBoxLayout(globalParam);
        verticalLayout_3->setObjectName(QString::fromUtf8("verticalLayout_3"));
        globalParamScroll = new QScrollArea(globalParam);
        globalParamScroll->setObjectName(QString::fromUtf8("globalParamScroll"));
        globalParamScroll->setWidgetResizable(true);
        globalParamScrollContents = new QWidget();
        globalParamScrollContents->setObjectName(QString::fromUtf8("globalParamScrollContents"));
        globalParamScrollContents->setGeometry(QRect(0, 0, 471, 567));
        gridLayout_2 = new QGridLayout(globalParamScrollContents);
        gridLayout_2->setObjectName(QString::fromUtf8("gridLayout_2"));
        intermediateParamCheckBox = new QCheckBox(globalParamScrollContents);
        intermediateParamCheckBox->setObjectName(QString::fromUtf8("intermediateParamCheckBox"));
        intermediateParamCheckBox->setChecked(true);
        intermediateParamCheckBox->setAutoExclusive(false);

        gridLayout_2->addWidget(intermediateParamCheckBox, 17, 1, 1, 1);

        keepAspectRatio = new QRadioButton(globalParamScrollContents);
        keepAspectRatio->setObjectName(QString::fromUtf8("keepAspectRatio"));
        keepAspectRatio->setChecked(true);

        gridLayout_2->addWidget(keepAspectRatio, 5, 1, 1, 1);

        lastFrameLabel = new QLabel(globalParamScrollContents);
        lastFrameLabel->setObjectName(QString::fromUtf8("lastFrameLabel"));
        QSizePolicy sizePolicy1(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
        sizePolicy1.setHorizontalStretch(0);
        sizePolicy1.setVerticalStretch(0);
        sizePolicy1.setHeightForWidth(lastFrameLabel->sizePolicy().hasHeightForWidth());
        lastFrameLabel->setSizePolicy(sizePolicy1);

        gridLayout_2->addWidget(lastFrameLabel, 21, 2, 1, 1);

        exportTypeLabel = new QLabel(globalParamScrollContents);
        exportTypeLabel->setObjectName(QString::fromUtf8("exportTypeLabel"));

        gridLayout_2->addWidget(exportTypeLabel, 9, 0, 1, 1);

        intermediateTypeLabel = new QLabel(globalParamScrollContents);
        intermediateTypeLabel->setObjectName(QString::fromUtf8("intermediateTypeLabel"));

        gridLayout_2->addWidget(intermediateTypeLabel, 12, 0, 1, 1);

        customRatio = new QRadioButton(globalParamScrollContents);
        customRatio->setObjectName(QString::fromUtf8("customRatio"));

        gridLayout_2->addWidget(customRatio, 5, 2, 1, 1);

        insertFrameRangeButton = new QPushButton(globalParamScrollContents);
        insertFrameRangeButton->setObjectName(QString::fromUtf8("insertFrameRangeButton"));
        QSizePolicy sizePolicy2(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sizePolicy2.setHorizontalStretch(0);
        sizePolicy2.setVerticalStretch(0);
        sizePolicy2.setHeightForWidth(insertFrameRangeButton->sizePolicy().hasHeightForWidth());
        insertFrameRangeButton->setSizePolicy(sizePolicy2);
        QIcon icon;
        icon.addFile(QString::fromUtf8("../../../shared/dev/AnimeEffects/data/themes/classic/icon/knockkey.png"), QSize(), QIcon::Normal, QIcon::Off);
        insertFrameRangeButton->setIcon(icon);

        gridLayout_2->addWidget(insertFrameRangeButton, 22, 0, 1, 1);

        exportLine1 = new QFrame(globalParamScrollContents);
        exportLine1->setObjectName(QString::fromUtf8("exportLine1"));
        exportLine1->setFrameShape(QFrame::HLine);
        exportLine1->setFrameShadow(QFrame::Sunken);

        gridLayout_2->addWidget(exportLine1, 8, 0, 1, 1);

        customPaletteCheckBox = new QCheckBox(globalParamScrollContents);
        customPaletteCheckBox->setObjectName(QString::fromUtf8("customPaletteCheckBox"));
        customPaletteCheckBox->setChecked(false);

        gridLayout_2->addWidget(customPaletteCheckBox, 18, 0, 1, 1);

        firstFrameSpinBox = new QSpinBox(globalParamScrollContents);
        firstFrameSpinBox->setObjectName(QString::fromUtf8("firstFrameSpinBox"));
        QSizePolicy sizePolicy3(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        sizePolicy3.setHorizontalStretch(0);
        sizePolicy3.setVerticalStretch(0);
        sizePolicy3.setHeightForWidth(firstFrameSpinBox->sizePolicy().hasHeightForWidth());
        firstFrameSpinBox->setSizePolicy(sizePolicy3);

        gridLayout_2->addWidget(firstFrameSpinBox, 22, 1, 1, 1);

        heightSpinBox = new QSpinBox(globalParamScrollContents);
        heightSpinBox->setObjectName(QString::fromUtf8("heightSpinBox"));
        heightSpinBox->setMaximum(9999999);
        heightSpinBox->setValue(720);

        gridLayout_2->addWidget(heightSpinBox, 4, 1, 1, 1);

        forePipeCheckBox = new QCheckBox(globalParamScrollContents);
        forePipeCheckBox->setObjectName(QString::fromUtf8("forePipeCheckBox"));

        gridLayout_2->addWidget(forePipeCheckBox, 14, 0, 1, 2);

        intermediateTypeCombo = new QComboBox(globalParamScrollContents);
        intermediateTypeCombo->addItem(QString());
        intermediateTypeCombo->addItem(QString());
        intermediateTypeCombo->addItem(QString());
        intermediateTypeCombo->setObjectName(QString::fromUtf8("intermediateTypeCombo"));

        gridLayout_2->addWidget(intermediateTypeCombo, 12, 1, 1, 2);

        widthLabel = new QLabel(globalParamScrollContents);
        widthLabel->setObjectName(QString::fromUtf8("widthLabel"));
        QSizePolicy sizePolicy4(QSizePolicy::Expanding, QSizePolicy::Preferred);
        sizePolicy4.setHorizontalStretch(0);
        sizePolicy4.setVerticalStretch(0);
        sizePolicy4.setHeightForWidth(widthLabel->sizePolicy().hasHeightForWidth());
        widthLabel->setSizePolicy(sizePolicy4);

        gridLayout_2->addWidget(widthLabel, 3, 0, 1, 1);

        oneToOneRatio = new QRadioButton(globalParamScrollContents);
        oneToOneRatio->setObjectName(QString::fromUtf8("oneToOneRatio"));
        oneToOneRatio->setChecked(false);

        gridLayout_2->addWidget(oneToOneRatio, 5, 0, 1, 1);

        setWidthNative = new QPushButton(globalParamScrollContents);
        setWidthNative->setObjectName(QString::fromUtf8("setWidthNative"));
        QIcon icon1;
        icon1.addFile(QString::fromUtf8("../../../shared/dev/AnimeEffects/data/themes/breeze_dark/icon/rotatecw.png"), QSize(), QIcon::Normal, QIcon::Off);
        setWidthNative->setIcon(icon1);

        gridLayout_2->addWidget(setWidthNative, 3, 2, 1, 1);

        renderLine1 = new QFrame(globalParamScrollContents);
        renderLine1->setObjectName(QString::fromUtf8("renderLine1"));
        sizePolicy3.setHeightForWidth(renderLine1->sizePolicy().hasHeightForWidth());
        renderLine1->setSizePolicy(sizePolicy3);
        renderLine1->setFrameShape(QFrame::HLine);
        renderLine1->setFrameShadow(QFrame::Sunken);

        gridLayout_2->addWidget(renderLine1, 2, 0, 1, 1);

        exportLine2 = new QFrame(globalParamScrollContents);
        exportLine2->setObjectName(QString::fromUtf8("exportLine2"));
        exportLine2->setFrameShape(QFrame::HLine);
        exportLine2->setFrameShadow(QFrame::Sunken);

        gridLayout_2->addWidget(exportLine2, 8, 2, 1, 1);

        postParamCheckBox = new QCheckBox(globalParamScrollContents);
        postParamCheckBox->setObjectName(QString::fromUtf8("postParamCheckBox"));
        postParamCheckBox->setChecked(true);
        postParamCheckBox->setAutoExclusive(false);

        gridLayout_2->addWidget(postParamCheckBox, 17, 2, 1, 1);

        paramsLabel = new QLabel(globalParamScrollContents);
        paramsLabel->setObjectName(QString::fromUtf8("paramsLabel"));

        gridLayout_2->addWidget(paramsLabel, 17, 0, 1, 1);

        widthSpinBox = new QSpinBox(globalParamScrollContents);
        widthSpinBox->setObjectName(QString::fromUtf8("widthSpinBox"));
        QSizePolicy sizePolicy5(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sizePolicy5.setHorizontalStretch(0);
        sizePolicy5.setVerticalStretch(0);
        sizePolicy5.setHeightForWidth(widthSpinBox->sizePolicy().hasHeightForWidth());
        widthSpinBox->setSizePolicy(sizePolicy5);
        widthSpinBox->setMinimum(1080);
        widthSpinBox->setMaximum(999999);

        gridLayout_2->addWidget(widthSpinBox, 3, 1, 1, 1);

        heightLabel = new QLabel(globalParamScrollContents);
        heightLabel->setObjectName(QString::fromUtf8("heightLabel"));
        sizePolicy4.setHeightForWidth(heightLabel->sizePolicy().hasHeightForWidth());
        heightLabel->setSizePolicy(sizePolicy4);

        gridLayout_2->addWidget(heightLabel, 4, 0, 1, 1);

        exportingLabel = new QLabel(globalParamScrollContents);
        exportingLabel->setObjectName(QString::fromUtf8("exportingLabel"));
        QSizePolicy sizePolicy6(QSizePolicy::Minimum, QSizePolicy::Maximum);
        sizePolicy6.setHorizontalStretch(0);
        sizePolicy6.setVerticalStretch(0);
        sizePolicy6.setHeightForWidth(exportingLabel->sizePolicy().hasHeightForWidth());
        exportingLabel->setSizePolicy(sizePolicy6);

        gridLayout_2->addWidget(exportingLabel, 8, 1, 1, 1);

        setHeightNative = new QPushButton(globalParamScrollContents);
        setHeightNative->setObjectName(QString::fromUtf8("setHeightNative"));
        setHeightNative->setIcon(icon1);

        gridLayout_2->addWidget(setHeightNative, 4, 2, 1, 1);

        fpsLabel = new QLabel(globalParamScrollContents);
        fpsLabel->setObjectName(QString::fromUtf8("fpsLabel"));

        gridLayout_2->addWidget(fpsLabel, 10, 0, 1, 1);

        renderingLabel = new QLabel(globalParamScrollContents);
        renderingLabel->setObjectName(QString::fromUtf8("renderingLabel"));
        QSizePolicy sizePolicy7(QSizePolicy::Preferred, QSizePolicy::Maximum);
        sizePolicy7.setHorizontalStretch(0);
        sizePolicy7.setVerticalStretch(0);
        sizePolicy7.setHeightForWidth(renderingLabel->sizePolicy().hasHeightForWidth());
        renderingLabel->setSizePolicy(sizePolicy7);

        gridLayout_2->addWidget(renderingLabel, 2, 1, 1, 1);

        exportTypeCombo = new QComboBox(globalParamScrollContents);
        QIcon icon2;
        icon2.addFile(QString::fromUtf8("../Downloads/thing.png"), QSize(), QIcon::Normal, QIcon::Off);
        exportTypeCombo->addItem(icon2, QString());
        QIcon icon3;
        icon3.addFile(QString::fromUtf8("../../../shared/dev/AnimeEffects/data/themes/classic/icon/cutimages.png"), QSize(), QIcon::Normal, QIcon::Off);
        exportTypeCombo->addItem(icon3, QString());
        exportTypeCombo->setObjectName(QString::fromUtf8("exportTypeCombo"));

        gridLayout_2->addWidget(exportTypeCombo, 9, 1, 1, 2);

        transparencyCheckBox = new QCheckBox(globalParamScrollContents);
        transparencyCheckBox->setObjectName(QString::fromUtf8("transparencyCheckBox"));
        transparencyCheckBox->setChecked(true);

        gridLayout_2->addWidget(transparencyCheckBox, 13, 0, 1, 2);

        lastFrameSpinBox = new QSpinBox(globalParamScrollContents);
        lastFrameSpinBox->setObjectName(QString::fromUtf8("lastFrameSpinBox"));
        sizePolicy3.setHeightForWidth(lastFrameSpinBox->sizePolicy().hasHeightForWidth());
        lastFrameSpinBox->setSizePolicy(sizePolicy3);

        gridLayout_2->addWidget(lastFrameSpinBox, 22, 2, 1, 1);

        bitrateLineEdit = new QLineEdit(globalParamScrollContents);
        bitrateLineEdit->setObjectName(QString::fromUtf8("bitrateLineEdit"));

        gridLayout_2->addWidget(bitrateLineEdit, 11, 1, 1, 2);

        renderedFramesLabel = new QLabel(globalParamScrollContents);
        renderedFramesLabel->setObjectName(QString::fromUtf8("renderedFramesLabel"));
        sizePolicy7.setHeightForWidth(renderedFramesLabel->sizePolicy().hasHeightForWidth());
        renderedFramesLabel->setSizePolicy(sizePolicy7);

        gridLayout_2->addWidget(renderedFramesLabel, 21, 0, 1, 1);

        renderLine2 = new QFrame(globalParamScrollContents);
        renderLine2->setObjectName(QString::fromUtf8("renderLine2"));
        sizePolicy3.setHeightForWidth(renderLine2->sizePolicy().hasHeightForWidth());
        renderLine2->setSizePolicy(sizePolicy3);
        renderLine2->setFrameShape(QFrame::HLine);
        renderLine2->setFrameShadow(QFrame::Sunken);

        gridLayout_2->addWidget(renderLine2, 2, 2, 1, 1);

        firstFrameLabel = new QLabel(globalParamScrollContents);
        firstFrameLabel->setObjectName(QString::fromUtf8("firstFrameLabel"));
        sizePolicy1.setHeightForWidth(firstFrameLabel->sizePolicy().hasHeightForWidth());
        firstFrameLabel->setSizePolicy(sizePolicy1);

        gridLayout_2->addWidget(firstFrameLabel, 21, 1, 1, 1);

        bitrateLabel = new QLabel(globalParamScrollContents);
        bitrateLabel->setObjectName(QString::fromUtf8("bitrateLabel"));

        gridLayout_2->addWidget(bitrateLabel, 11, 0, 1, 1);

        allowParamsCheckBox = new QCheckBox(globalParamScrollContents);
        allowParamsCheckBox->setObjectName(QString::fromUtf8("allowParamsCheckBox"));
        allowParamsCheckBox->setChecked(true);

        gridLayout_2->addWidget(allowParamsCheckBox, 16, 0, 1, 3);

        fpsCombo = new QSpinBox(globalParamScrollContents);
        fpsCombo->setObjectName(QString::fromUtf8("fpsCombo"));
        fpsCombo->setMaximum(144);
        fpsCombo->setValue(24);

        gridLayout_2->addWidget(fpsCombo, 10, 1, 1, 2);

        selectPalettePathButton = new QToolButton(globalParamScrollContents);
        selectPalettePathButton->setObjectName(QString::fromUtf8("selectPalettePathButton"));
        sizePolicy2.setHeightForWidth(selectPalettePathButton->sizePolicy().hasHeightForWidth());
        selectPalettePathButton->setSizePolicy(sizePolicy2);

        gridLayout_2->addWidget(selectPalettePathButton, 18, 1, 1, 2);

        globalParamScroll->setWidget(globalParamScrollContents);

        verticalLayout_3->addWidget(globalParamScroll);

        settingsTab->addTab(globalParam, QString());
        formatParam = new QWidget();
        formatParam->setObjectName(QString::fromUtf8("formatParam"));
        verticalLayout_4 = new QVBoxLayout(formatParam);
        verticalLayout_4->setObjectName(QString::fromUtf8("verticalLayout_4"));
        formatLayout = new QFormLayout();
        formatLayout->setObjectName(QString::fromUtf8("formatLayout"));
        formatParamScroll = new QScrollArea(formatParam);
        formatParamScroll->setObjectName(QString::fromUtf8("formatParamScroll"));
        formatParamScroll->setWidgetResizable(true);
        formatParamScrollContents = new QWidget();
        formatParamScrollContents->setObjectName(QString::fromUtf8("formatParamScrollContents"));
        formatParamScrollContents->setGeometry(QRect(0, 0, 189, 199));
        gridLayout = new QGridLayout(formatParamScrollContents);
        gridLayout->setObjectName(QString::fromUtf8("gridLayout"));
        pixelFormatLabel = new QLabel(formatParamScrollContents);
        pixelFormatLabel->setObjectName(QString::fromUtf8("pixelFormatLabel"));

        gridLayout->addWidget(pixelFormatLabel, 0, 0, 1, 1);

        movLabel = new QLabel(formatParamScrollContents);
        movLabel->setObjectName(QString::fromUtf8("movLabel"));

        gridLayout->addWidget(movLabel, 4, 0, 1, 1);

        aviCombo = new QComboBox(formatParamScrollContents);
        aviCombo->addItem(QString());
        aviCombo->addItem(QString());
        aviCombo->addItem(QString());
        aviCombo->setObjectName(QString::fromUtf8("aviCombo"));

        gridLayout->addWidget(aviCombo, 2, 1, 1, 2);

        mkvLabel = new QLabel(formatParamScrollContents);
        mkvLabel->setObjectName(QString::fromUtf8("mkvLabel"));

        gridLayout->addWidget(mkvLabel, 3, 0, 1, 1);

        pixelFormatCombo = new QComboBox(formatParamScrollContents);
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->addItem(QString());
        pixelFormatCombo->setObjectName(QString::fromUtf8("pixelFormatCombo"));

        gridLayout->addWidget(pixelFormatCombo, 0, 1, 1, 2);

        movCombo = new QComboBox(formatParamScrollContents);
        movCombo->addItem(QString());
        movCombo->addItem(QString());
        movCombo->addItem(QString());
        movCombo->addItem(QString());
        movCombo->addItem(QString());
        movCombo->setObjectName(QString::fromUtf8("movCombo"));

        gridLayout->addWidget(movCombo, 4, 1, 1, 2);

        formatLine = new QFrame(formatParamScrollContents);
        formatLine->setObjectName(QString::fromUtf8("formatLine"));
        formatLine->setFrameShape(QFrame::HLine);
        formatLine->setFrameShadow(QFrame::Sunken);

        gridLayout->addWidget(formatLine, 1, 0, 1, 3);

        mp4Label = new QLabel(formatParamScrollContents);
        mp4Label->setObjectName(QString::fromUtf8("mp4Label"));

        gridLayout->addWidget(mp4Label, 5, 0, 1, 1);

        aviLabel = new QLabel(formatParamScrollContents);
        aviLabel->setObjectName(QString::fromUtf8("aviLabel"));
        sizePolicy7.setHeightForWidth(aviLabel->sizePolicy().hasHeightForWidth());
        aviLabel->setSizePolicy(sizePolicy7);

        gridLayout->addWidget(aviLabel, 2, 0, 1, 1);

        webmCombo = new QComboBox(formatParamScrollContents);
        webmCombo->addItem(QString());
        webmCombo->addItem(QString());
        webmCombo->addItem(QString());
        webmCombo->setObjectName(QString::fromUtf8("webmCombo"));

        gridLayout->addWidget(webmCombo, 7, 1, 1, 2);

        mp4Combo = new QComboBox(formatParamScrollContents);
        mp4Combo->addItem(QString());
        mp4Combo->addItem(QString());
        mp4Combo->addItem(QString());
        mp4Combo->setObjectName(QString::fromUtf8("mp4Combo"));

        gridLayout->addWidget(mp4Combo, 5, 1, 1, 2);

        webmLabel = new QLabel(formatParamScrollContents);
        webmLabel->setObjectName(QString::fromUtf8("webmLabel"));

        gridLayout->addWidget(webmLabel, 7, 0, 1, 1);

        mkvCombo = new QComboBox(formatParamScrollContents);
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->addItem(QString());
        mkvCombo->setObjectName(QString::fromUtf8("mkvCombo"));

        gridLayout->addWidget(mkvCombo, 3, 1, 1, 2);

        formatParamScroll->setWidget(formatParamScrollContents);

        formatLayout->setWidget(0, QFormLayout::SpanningRole, formatParamScroll);


        verticalLayout_4->addLayout(formatLayout);

        settingsTab->addTab(formatParam, QString());
        customParam = new QWidget();
        customParam->setObjectName(QString::fromUtf8("customParam"));
        verticalLayout_5 = new QVBoxLayout(customParam);
        verticalLayout_5->setObjectName(QString::fromUtf8("verticalLayout_5"));
        customParamLayout = new QGridLayout();
        customParamLayout->setObjectName(QString::fromUtf8("customParamLayout"));
        intermediateLabel = new QLabel(customParam);
        intermediateLabel->setObjectName(QString::fromUtf8("intermediateLabel"));
        QSizePolicy sizePolicy8(QSizePolicy::Maximum, QSizePolicy::Preferred);
        sizePolicy8.setHorizontalStretch(0);
        sizePolicy8.setVerticalStretch(0);
        sizePolicy8.setHeightForWidth(intermediateLabel->sizePolicy().hasHeightForWidth());
        intermediateLabel->setSizePolicy(sizePolicy8);
        intermediateLabel->setWordWrap(true);

        customParamLayout->addWidget(intermediateLabel, 1, 0, 1, 1);

        postLabel = new QLabel(customParam);
        postLabel->setObjectName(QString::fromUtf8("postLabel"));

        customParamLayout->addWidget(postLabel, 3, 0, 1, 1);

        customLine = new QFrame(customParam);
        customLine->setObjectName(QString::fromUtf8("customLine"));
        customLine->setFrameShape(QFrame::HLine);
        customLine->setFrameShadow(QFrame::Sunken);

        customParamLayout->addWidget(customLine, 5, 0, 1, 3);

        addPresetToIntermediate = new QPushButton(customParam);
        addPresetToIntermediate->setObjectName(QString::fromUtf8("addPresetToIntermediate"));

        customParamLayout->addWidget(addPresetToIntermediate, 7, 1, 1, 1);

        AddPressetToPost = new QPushButton(customParam);
        AddPressetToPost->setObjectName(QString::fromUtf8("AddPressetToPost"));

        customParamLayout->addWidget(AddPressetToPost, 7, 2, 1, 1);

        presetLabel = new QLabel(customParam);
        presetLabel->setObjectName(QString::fromUtf8("presetLabel"));

        customParamLayout->addWidget(presetLabel, 6, 0, 1, 1);

        presetCombo = new QComboBox(customParam);
        presetCombo->addItem(QString());
        presetCombo->addItem(QString());
        presetCombo->setObjectName(QString::fromUtf8("presetCombo"));

        customParamLayout->addWidget(presetCombo, 6, 1, 1, 3);

        removePreset = new QPushButton(customParam);
        removePreset->setObjectName(QString::fromUtf8("removePreset"));

        customParamLayout->addWidget(removePreset, 7, 3, 1, 1);

        postParamTextEdit = new QTextEdit(customParam);
        postParamTextEdit->setObjectName(QString::fromUtf8("postParamTextEdit"));
        QSizePolicy sizePolicy9(QSizePolicy::MinimumExpanding, QSizePolicy::Expanding);
        sizePolicy9.setHorizontalStretch(0);
        sizePolicy9.setVerticalStretch(0);
        sizePolicy9.setHeightForWidth(postParamTextEdit->sizePolicy().hasHeightForWidth());
        postParamTextEdit->setSizePolicy(sizePolicy9);

        customParamLayout->addWidget(postParamTextEdit, 3, 1, 1, 3);

        savePostParamAsPreset = new QToolButton(customParam);
        savePostParamAsPreset->setObjectName(QString::fromUtf8("savePostParamAsPreset"));
        sizePolicy3.setHeightForWidth(savePostParamAsPreset->sizePolicy().hasHeightForWidth());
        savePostParamAsPreset->setSizePolicy(sizePolicy3);

        customParamLayout->addWidget(savePostParamAsPreset, 4, 1, 1, 3);

        intermediateParamTextEdit = new QTextEdit(customParam);
        intermediateParamTextEdit->setObjectName(QString::fromUtf8("intermediateParamTextEdit"));

        customParamLayout->addWidget(intermediateParamTextEdit, 1, 1, 1, 3);

        saveIntermediateParamAsPreset = new QToolButton(customParam);
        saveIntermediateParamAsPreset->setObjectName(QString::fromUtf8("saveIntermediateParamAsPreset"));
        sizePolicy3.setHeightForWidth(saveIntermediateParamAsPreset->sizePolicy().hasHeightForWidth());
        saveIntermediateParamAsPreset->setSizePolicy(sizePolicy3);

        customParamLayout->addWidget(saveIntermediateParamAsPreset, 2, 1, 1, 3);

        commandsLabel = new QLabel(customParam);
        commandsLabel->setObjectName(QString::fromUtf8("commandsLabel"));

        customParamLayout->addWidget(commandsLabel, 0, 0, 1, 4);


        verticalLayout_5->addLayout(customParamLayout);

        settingsTab->addTab(customParam, QString());

        tabVBox->addWidget(settingsTab);


        verticalLayout_2->addLayout(tabVBox);

        exportLayout = new QHBoxLayout();
        exportLayout->setObjectName(QString::fromUtf8("exportLayout"));
        exportButton = new QPushButton(exportWidget);
        exportButton->setObjectName(QString::fromUtf8("exportButton"));

        exportLayout->addWidget(exportButton);

        cancelButton = new QPushButton(exportWidget);
        cancelButton->setObjectName(QString::fromUtf8("cancelButton"));

        exportLayout->addWidget(cancelButton);


        verticalLayout_2->addLayout(exportLayout);


        retranslateUi(exportWidget);

        settingsTab->setCurrentIndex(0);


        QMetaObject::connectSlotsByName(exportWidget);
    } // setupUi

    void retranslateUi(QWidget *exportWidget)
    {
        exportWidget->setWindowTitle(QCoreApplication::translate("exportWidget", "Export parameters", nullptr));
        intermediateParamCheckBox->setText(QCoreApplication::translate("exportWidget", "Intermediate ", nullptr));
        keepAspectRatio->setText(QCoreApplication::translate("exportWidget", "Keep aspect ratio", nullptr));
        lastFrameLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">- Last frame -</p></body></html>", nullptr));
        exportTypeLabel->setText(QCoreApplication::translate("exportWidget", "Export type:", nullptr));
        intermediateTypeLabel->setText(QCoreApplication::translate("exportWidget", "Intermediate type:", nullptr));
        customRatio->setText(QCoreApplication::translate("exportWidget", "Custom", nullptr));
        insertFrameRangeButton->setText(QCoreApplication::translate("exportWidget", "Insert range", nullptr));
        customPaletteCheckBox->setText(QCoreApplication::translate("exportWidget", "Custom palette", nullptr));
        forePipeCheckBox->setText(QCoreApplication::translate("exportWidget", "Force piped export ", nullptr));
        intermediateTypeCombo->setItemText(0, QCoreApplication::translate("exportWidget", "PNG", nullptr));
        intermediateTypeCombo->setItemText(1, QCoreApplication::translate("exportWidget", "BMP", nullptr));
        intermediateTypeCombo->setItemText(2, QCoreApplication::translate("exportWidget", "JPEG", nullptr));

        widthLabel->setText(QCoreApplication::translate("exportWidget", "Width: ", nullptr));
        oneToOneRatio->setText(QCoreApplication::translate("exportWidget", "One-to-one", nullptr));
        setWidthNative->setText(QCoreApplication::translate("exportWidget", "Set to native", nullptr));
        postParamCheckBox->setText(QCoreApplication::translate("exportWidget", "Post", nullptr));
        paramsLabel->setText(QCoreApplication::translate("exportWidget", "Active parameters:", nullptr));
        heightLabel->setText(QCoreApplication::translate("exportWidget", "Height:", nullptr));
        exportingLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">Exporting</p></body></html>", nullptr));
        setHeightNative->setText(QCoreApplication::translate("exportWidget", "Set to native", nullptr));
        fpsLabel->setText(QCoreApplication::translate("exportWidget", "FPS:", nullptr));
        renderingLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">Rendering</p></body></html>", nullptr));
        exportTypeCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Animation", nullptr));
        exportTypeCombo->setItemText(1, QCoreApplication::translate("exportWidget", "Image sequence", nullptr));

        transparencyCheckBox->setText(QCoreApplication::translate("exportWidget", "Allow transparency when supported", nullptr));
        bitrateLineEdit->setPlaceholderText(QCoreApplication::translate("exportWidget", "Auto", nullptr));
        renderedFramesLabel->setText(QCoreApplication::translate("exportWidget", "Frames rendered: ", nullptr));
        firstFrameLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">- First frame -</p></body></html>", nullptr));
        bitrateLabel->setText(QCoreApplication::translate("exportWidget", "Bitrate (kB/s):", nullptr));
        allowParamsCheckBox->setText(QCoreApplication::translate("exportWidget", "Export with custom parameters", nullptr));
        selectPalettePathButton->setText(QCoreApplication::translate("exportWidget", "Select palette path", nullptr));
        settingsTab->setTabText(settingsTab->indexOf(globalParam), QCoreApplication::translate("exportWidget", "General Parameters", nullptr));
        pixelFormatLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p>Pixel format:</p></body></html>", nullptr));
        movLabel->setText(QCoreApplication::translate("exportWidget", "MOV", nullptr));
        aviCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Auto", nullptr));
        aviCombo->setItemText(1, QCoreApplication::translate("exportWidget", "MPEG-4", nullptr));
        aviCombo->setItemText(2, QCoreApplication::translate("exportWidget", "MPEG-2", nullptr));

        mkvLabel->setText(QCoreApplication::translate("exportWidget", "MKV", nullptr));
        pixelFormatCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Auto", nullptr));
        pixelFormatCombo->setItemText(1, QCoreApplication::translate("exportWidget", "yuv410p", nullptr));
        pixelFormatCombo->setItemText(2, QCoreApplication::translate("exportWidget", "yuv410p", nullptr));
        pixelFormatCombo->setItemText(3, QCoreApplication::translate("exportWidget", "yuv420p", nullptr));
        pixelFormatCombo->setItemText(4, QCoreApplication::translate("exportWidget", "yuv422p", nullptr));
        pixelFormatCombo->setItemText(5, QCoreApplication::translate("exportWidget", "yuv444p", nullptr));
        pixelFormatCombo->setItemText(6, QCoreApplication::translate("exportWidget", "yuyv422", nullptr));
        pixelFormatCombo->setItemText(7, QCoreApplication::translate("exportWidget", "rgb24", nullptr));
        pixelFormatCombo->setItemText(8, QCoreApplication::translate("exportWidget", "bgr24", nullptr));
        pixelFormatCombo->setItemText(9, QCoreApplication::translate("exportWidget", "gray", nullptr));

    #if QT_CONFIG(tooltip)
        pixelFormatCombo->setToolTip(QCoreApplication::translate("exportWidget", "Pixel Format availability depends on the selected format, we recommend you stick to yuv420p", nullptr));
    #endif // QT_CONFIG(tooltip)
        movCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Auto", nullptr));
        movCombo->setItemText(1, QCoreApplication::translate("exportWidget", "libx264", nullptr));
        movCombo->setItemText(2, QCoreApplication::translate("exportWidget", "h264", nullptr));
        movCombo->setItemText(3, QCoreApplication::translate("exportWidget", "prores_ks", nullptr));
        movCombo->setItemText(4, QCoreApplication::translate("exportWidget", "utvideo", nullptr));

        mp4Label->setText(QCoreApplication::translate("exportWidget", "MP4", nullptr));
        aviLabel->setText(QCoreApplication::translate("exportWidget", "AVI", nullptr));
        webmCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Auto", nullptr));
        webmCombo->setItemText(1, QCoreApplication::translate("exportWidget", "VP8", nullptr));
        webmCombo->setItemText(2, QCoreApplication::translate("exportWidget", "VP9", nullptr));

        mp4Combo->setItemText(0, QCoreApplication::translate("exportWidget", "Auto", nullptr));
        mp4Combo->setItemText(1, QCoreApplication::translate("exportWidget", "MPEG-4", nullptr));
        mp4Combo->setItemText(2, QCoreApplication::translate("exportWidget", "H264", nullptr));

        webmLabel->setText(QCoreApplication::translate("exportWidget", "WEBM", nullptr));
        mkvCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Auto", nullptr));
        mkvCombo->setItemText(1, QCoreApplication::translate("exportWidget", "x264", nullptr));
        mkvCombo->setItemText(2, QCoreApplication::translate("exportWidget", "x265", nullptr));
        mkvCombo->setItemText(3, QCoreApplication::translate("exportWidget", "VP8", nullptr));
        mkvCombo->setItemText(4, QCoreApplication::translate("exportWidget", "VP9", nullptr));
        mkvCombo->setItemText(5, QCoreApplication::translate("exportWidget", "AV1", nullptr));
        mkvCombo->setItemText(6, QCoreApplication::translate("exportWidget", "FFV1", nullptr));
        mkvCombo->setItemText(7, QCoreApplication::translate("exportWidget", "MagicYUV", nullptr));
        mkvCombo->setItemText(8, QCoreApplication::translate("exportWidget", "HuffYUV", nullptr));
        mkvCombo->setItemText(9, QCoreApplication::translate("exportWidget", "Theora", nullptr));

        settingsTab->setTabText(settingsTab->indexOf(formatParam), QCoreApplication::translate("exportWidget", "Format Parameters", nullptr));
        intermediateLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">IIntermediate:</p></body></html>", nullptr));
        postLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">Post:</p></body></html>", nullptr));
        addPresetToIntermediate->setText(QCoreApplication::translate("exportWidget", "Add to Intermediate pass", nullptr));
        AddPressetToPost->setText(QCoreApplication::translate("exportWidget", "Add to Post", nullptr));
        presetLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">Presets:</p></body></html>", nullptr));
        presetCombo->setItemText(0, QCoreApplication::translate("exportWidget", "Compress animation", nullptr));
        presetCombo->setItemText(1, QCoreApplication::translate("exportWidget", "Compress image sequence", nullptr));

        removePreset->setText(QCoreApplication::translate("exportWidget", "Remove preset", nullptr));
        savePostParamAsPreset->setText(QCoreApplication::translate("exportWidget", "Save as preset...", nullptr));
        saveIntermediateParamAsPreset->setText(QCoreApplication::translate("exportWidget", "Save as preset...", nullptr));
    #if QT_CONFIG(tooltip)
        commandsLabel->setToolTip(QCoreApplication::translate("exportWidget", "<html><head/><body><p>You may use these special attributes in your custom command:</p><ul><p><li> %exportfilename%</li></p><p><li> %intermediatefilename%</p></li><p><li> %fps%</p></li><p><li> %palettepath%</p></li><p><li> %exportpath%</p></li><p><li> %intermediatepath%</p></li></ul></body></html>", nullptr));
    #endif // QT_CONFIG(tooltip)
        commandsLabel->setText(QCoreApplication::translate("exportWidget", "<html><head/><body><p align=\"center\">Commands</p></body></html>", nullptr));
        settingsTab->setTabText(settingsTab->indexOf(customParam), QCoreApplication::translate("exportWidget", "Custom Parameters", nullptr));
        exportButton->setText(QCoreApplication::translate("exportWidget", "Export", nullptr));
        cancelButton->setText(QCoreApplication::translate("exportWidget", "Cancel", nullptr));
    } // retranslateUi

};

namespace Ui {
class exportWidget: public ExportWidgetUI {};
} // namespace Ui

QT_END_NAMESPACE

#endif // EXPORTDIAG_H
