#include "QBootablesView.h"
#include "ui_bootableview.h"

#include <QAction>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>

#include "AppConfig.h"
#include "CoverUtils.h"
#include "http/HttpClientFactory.h"
#include "string_format.h"
#include "QStringUtils.h"
#include "QtUtils.h"
#include "ui_shared/BootablesProcesses.h"

#ifdef HAS_AMAZON_S3
#include "S3FileBrowser.h"
#include "amazon/AmazonS3Client.h"
#include "../s3stream/S3ObjectStream.h"
#include "ui_shared/AmazonS3Utils.h"
#endif

QBootablesView::QBootablesView(QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::QBootablesView)

{
	ui->setupUi(this);
	ui->listView->setStyleSheet("QListView{ background:QLinearGradient( x1: 0, y1: 0, x2: 0, y2: 1, stop: 0 #4baaf3, stop: 1 #0A0A0A); }");
	m_proxyModel = new BootableModelProxy(this);
	ui->listView->setModel(m_proxyModel);

	CAppConfig::GetInstance().RegisterPreferenceInteger("ui.sortmethod", 2);
	m_sortingMethod = CAppConfig::GetInstance().GetPreferenceInteger("ui.sortmethod");
	ui->comboBox->setCurrentIndex(m_sortingMethod);

	connect(ui->filterLineEdit, &QLineEdit::textChanged, m_proxyModel, &QSortFilterProxyModel::setFilterFixedString);
	connect(ui->listView->selectionModel(), &QItemSelectionModel::currentChanged, this, &QBootablesView::SelectionChange);
	connect(ui->listView, &QAbstractItemView::doubleClicked, this, &QBootablesView::DoubleClicked);
	connect(this, &QBootablesView::AsyncUpdateStatus, this, &QBootablesView::UpdateStatus);

	// used as workaround to avoid direct ui access from a thread
	connect(this, SIGNAL(AsyncUpdateCoverDisplay()), this, SLOT(UpdateCoverDisplay()));
	connect(this, SIGNAL(AsyncResetModel(bool)), this, SLOT(resetModel(bool)));

	//if m_sortingMethod == currentIndex == 0, setting index wont trigger on_comboBox_currentIndexChanged() thus resetModel()
	if(m_sortingMethod == 0)
	{
		resetModel();
	}

	ui->listView->setContextMenuPolicy(Qt::ActionsContextMenu);
	ui->listView->setItemDelegate(new BootImageItemDelegate);

	CoverUtils::PopulatePlaceholderCover();

#ifdef HAS_AMAZON_S3
	m_continuationChecker = new CContinuationChecker(this);
	ui->awsS3Button->setVisible(S3FileBrowser::IsAvailable());
#endif
}

QBootablesView::~QBootablesView()
{
	if(m_coverLoader.joinable())
		m_coverLoader.join();
}

void QBootablesView::AddMsgLabel(ElidedLabel* msgLabel)
{
	m_msgLabel = msgLabel;
}

void QBootablesView::SetupActions(BootCallback bootCallback)
{
	auto bootAction = new QAction("Boot", this);
	auto listViewBoundCallback = [listView = ui->listView, proxyModel = m_proxyModel, bootCallback]() {
		auto index = listView->selectionModel()->selectedIndexes().at(0);
		auto src_index = proxyModel->mapToSource(index);
		auto model = static_cast<BootableModel*>(proxyModel->sourceModel());
		auto bootable = model->GetBootable(src_index);

		bootCallback(bootable.path);
	};
	connect(bootAction, &QAction::triggered, listViewBoundCallback);
	ui->listView->addAction(bootAction);
	m_bootCallback = bootCallback;

	auto removeAction = new QAction("Remove", this);
	auto removeGameCallback = [listView = ui->listView, proxyModel = m_proxyModel](bool) {
		auto index = listView->selectionModel()->selectedIndexes().at(0);
		auto src_index = proxyModel->mapToSource(index);
		auto model = static_cast<BootableModel*>(proxyModel->sourceModel());
		auto bootable = model->GetBootable(src_index);

		BootablesDb::CClient::GetInstance().UnregisterBootable(bootable.path);
		model->removeItem(index);
	};
	connect(removeAction, &QAction::triggered, removeGameCallback);
	ui->listView->addAction(removeAction);
}

void QBootablesView::DoubleClicked(const QModelIndex& index)
{
	auto src_index = m_proxyModel->mapToSource(index);
	auto model = static_cast<BootableModel*>(m_proxyModel->sourceModel());
	auto bootable = model->GetBootable(src_index);
	m_bootCallback(bootable.path);
}

void QBootablesView::AsyncPopulateCache()
{
	if(!m_threadRunning)
	{
		if(m_coverLoader.joinable())
			m_coverLoader.join();

		m_threadRunning = true;
		m_coverLoader = std::thread([&] {
			CoverUtils::PopulateCache(m_bootables);

			AsyncUpdateCoverDisplay();
			m_threadRunning = false;
		});
	}
}

void QBootablesView::resizeEvent(QResizeEvent* ev)
{
	static_cast<BootableModel*>(m_proxyModel->sourceModel())->SetWidth(size().width() - style()->pixelMetric(QStyle::PM_ScrollBarExtent) - 5);
	QWidget::resizeEvent(ev);
}

void QBootablesView::resetModel(bool repopulateBootables)
{
	if(repopulateBootables)
		m_bootables = BootablesDb::CClient::GetInstance().GetBootables(m_sortingMethod);

	auto oldModel = m_proxyModel->sourceModel();
	auto model = new BootableModel(this, m_bootables);
	m_proxyModel->setSourceModel(model);

	if(oldModel)
		delete oldModel;

	AsyncPopulateCache();
}

void QBootablesView::on_add_games_button_clicked()
{
	QStringList filters;
	filters.push_back(QtUtils::GetDiscImageFormatsFilter());
	filters.push_back("ELF files (*.elf)");
	filters.push_back("All files (*)");

	QFileDialog dialog(this);
	dialog.setFileMode(QFileDialog::ExistingFile);
	dialog.setNameFilters(filters);
	if(dialog.exec())
	{
		auto filePath = QStringToPath(dialog.selectedFiles().first()).parent_path();
		try
		{
			ScanBootables(filePath, false);
		}
		catch(...)
		{
		}
		FetchGameTitles();
		FetchGameCovers();
		resetModel();
	}
}

void QBootablesView::BootBootables(const QModelIndex& index)
{
	auto src_index = m_proxyModel->mapToSource(index);
	assert(src_index.isValid());
	auto bootable = static_cast<BootableModel*>(m_proxyModel->sourceModel())->GetBootable(index);
	m_bootCallback(bootable.path);
}

void QBootablesView::on_listView_doubleClicked(const QModelIndex& index)
{
	BootBootables(index);
}

void QBootablesView::on_refresh_button_clicked()
{
	auto bootables_paths = GetActiveBootableDirectories();
	for(auto path : bootables_paths)
	{
		try
		{
			ScanBootables(path, false);
		}
		catch(...)
		{
		}
	}
	FetchGameTitles();
	FetchGameCovers();

	resetModel();
}

void QBootablesView::on_comboBox_currentIndexChanged(int index)
{
	CAppConfig::GetInstance().SetPreferenceInteger("ui.sortmethod", index);
	m_sortingMethod = index;
	resetModel();
}

void QBootablesView::on_awsS3Button_clicked()
{
#ifdef HAS_AMAZON_S3
	std::string bucketName = CAppConfig::GetInstance().GetPreferenceString("s3.filebrowser.bucketname");
	{
		bool ok;
		QString res = QInputDialog::getText(this, tr("New Function"),
		                                    tr("New Function Name:"), QLineEdit::Normal,
		                                    bucketName.c_str(), &ok);
		if(!ok || res.isEmpty())
			return;

		bucketName = res.toStdString();
	}
	if(bucketName.empty())
		return;

	auto getListFuture = std::async(std::launch::async, [this, bucketName]() {
		m_s3Processing = true;
		auto credentials = CS3ObjectStream::CConfig::GetInstance().GetCredentials();
		AsyncUpdateStatus("Requesting S3 Bucket Content.");
		auto result = AmazonS3Utils::GetListObjects(credentials, bucketName);
		auto size = result.objects.size();
		int i = 1;
		bool new_entry = false;
		for(const auto& item : result.objects)
		{
			auto path = string_format("//s3/%s/%s", bucketName.c_str(), item.key.c_str());
			try
			{
				std::string msg = string_format("Processing: %s (%d/%d)", path.c_str(), i, size);
				AsyncUpdateStatus(msg);
				new_entry |= TryRegisterBootable(path);
			}
			catch(const std::exception& exception)
			{
				//Failed to process a path, keep going
			}
			++i;
		}
		return new_entry;
	});

	auto updateBootableCallback = [this](bool new_entry) {
		if(new_entry)
		{
			AsyncUpdateStatus("Refreshing Model.");
			resetModel();
		}
		m_s3Processing = false;
		AsyncUpdateStatus("Complete.");
	};
	m_continuationChecker->GetContinuationManager().Register(std::move(getListFuture), updateBootableCallback);
#endif
}

void QBootablesView::SelectionChange(const QModelIndex& index)
{
	auto src_index = m_proxyModel->mapToSource(index);
	if(src_index.isValid())
	{
		auto bootable = static_cast<BootableModel*>(m_proxyModel->sourceModel())->GetBootable(src_index);
		ui->pathLineEdit->setText(bootable.path.string().c_str());
		ui->serialLineEdit->setText(bootable.discId.c_str());
	}
	else
	{
		ui->pathLineEdit->clear();
		ui->serialLineEdit->clear();
	}
}

void QBootablesView::UpdateStatus(std::string msg)
{
	m_msgLabel->setText(msg.c_str());
}

void QBootablesView::DisplayWarningMessage()
{
	QMessageBox::warning(this, "Warning Message",
	                     "Can't close dialog while background operation in progress.",
	                     QMessageBox::Ok, QMessageBox::Ok);
}

void QBootablesView::on_reset_filter_button_clicked()
{
	ui->filterLineEdit->clear();
}
