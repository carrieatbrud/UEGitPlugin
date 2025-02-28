// Copyright (c) 2014-2022 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlMenu.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"
#include "GitSourceControlUtils.h"

#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"

#include "LevelEditor.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/MessageDialog.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
#include "Styling/AppStyle.h"
#else
#include "EditorStyleSet.h"
#endif

#include "PackageTools.h"
#include "FileHelpers.h"

#include "Logging/MessageLog.h"

#if ENGINE_MAJOR_VERSION == 5
#include "ToolMenus.h"
#include "ToolMenuContext.h"
#include "ToolMenuMisc.h"
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

void FGitSourceControlMenu::Register()
{
	// Register the extension with the level editor

#if ENGINE_MAJOR_VERSION == 5
	FToolMenuOwnerScoped SourceControlMenuOwner("GitSourceControlMenu");
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		UToolMenu* SourceControlMenu = ToolMenus->ExtendMenu("StatusBar.ToolBar.SourceControl");
		FToolMenuSection& Section = SourceControlMenu->AddSection("GitSourceControlActions", LOCTEXT("GitSourceControlMenuHeadingActions", "Git"), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));

		AddMenuExtension(Section);
	}
#else
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	LevelEditorModule)
	{
		FLevelEditorModule::FLevelEditorMenuExtender ViewMenuExtender = FLevelEditorModule::FLevelEditorMenuExtender::CreateRaw(this, &FGitSourceControlMenu::OnExtendLevelEditorViewMenu);
		auto& MenuExtenders = LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders();
		MenuExtenders.Add(ViewMenuExtender);
		ViewMenuExtenderHandle = MenuExtenders.Last().GetHandle();
	}
#endif
}

void FGitSourceControlMenu::Unregister()
{
	// Unregister the level editor extensions
#if ENGINE_MAJOR_VERSION == 5
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		UToolMenus::Get()->UnregisterOwnerByName("GitSourceControlMenu");
	}
#else
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (LevelEditorModule)
	{
		LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders().RemoveAll([=](const FLevelEditorModule::FLevelEditorMenuExtender& Extender) { return Extender.GetHandle() == ViewMenuExtenderHandle; });
	}
#endif
}

bool FGitSourceControlMenu::HaveRemoteUrl() const
{
	const FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
	const FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	return !Provider.GetRemoteUrl().IsEmpty();
}

/// Prompt to save or discard all packages
bool FGitSourceControlMenu::SaveDirtyPackages()
{
	const bool bPromptUserToSave = true;
	const bool bSaveMapPackages = true;
	const bool bSaveContentPackages = true;
	const bool bFastSave = false;
	const bool bNotifyNoPackagesSaved = false;
	const bool bCanBeDeclined = true; // If the user clicks "don't save" this will continue and lose their changes
	bool bHadPackagesToSave = false;

	bool bSaved = FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages, bFastSave, bNotifyNoPackagesSaved, bCanBeDeclined, &bHadPackagesToSave);

	// bSaved can be true if the user selects to not save an asset by unchecking it and clicking "save"
	if (bSaved)
	{
		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
		FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
		bSaved = DirtyPackages.Num() == 0;
	}

	return bSaved;
}

/// Find all packages in Content directory
TArray<FString> FGitSourceControlMenu::ListAllPackages()
{
	TArray<FString> PackageRelativePaths;
	FPackageName::FindPackagesInDirectory(PackageRelativePaths, *FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));

	TArray<FString> PackageNames;
	PackageNames.Reserve(PackageRelativePaths.Num());
	for (const FString& Path : PackageRelativePaths)
	{
		FString PackageName;
		FString FailureReason;
		if (FPackageName::TryConvertFilenameToLongPackageName(Path, PackageName, &FailureReason))
		{
			PackageNames.Add(PackageName);
		}
		else
		{
			FMessageLog("SourceControl").Error(FText::FromString(FailureReason));
		}
	}

	return PackageNames;
}

/// Unkink all loaded packages to allow to update them
TArray<UPackage*> FGitSourceControlMenu::UnlinkPackages(const TArray<FString>& InPackageNames)
{
	TArray<UPackage*> LoadedPackages;

	// Inspired from ContentBrowserUtils::SyncPathsFromSourceControl()
	if (InPackageNames.Num() > 0)
	{
		// Form a list of loaded packages to reload...
		LoadedPackages.Reserve(InPackageNames.Num());
		for (const FString& PackageName : InPackageNames)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (Package)
			{
				LoadedPackages.Emplace(Package);

				// Detach the linkers of any loaded packages so that SCC can overwrite the files...
				if (!Package->IsFullyLoaded())
				{
					FlushAsyncLoading();
					Package->FullyLoad();
				}
				ResetLoaders(Package);
			}
		}
		UE_LOG(LogSourceControl, Log, TEXT("Reseted Loader for %d Packages"), LoadedPackages.Num());
	}

	return LoadedPackages;
}

void FGitSourceControlMenu::ReloadPackages(TArray<UPackage*>& InPackagesToReload)
{
	UE_LOG(LogSourceControl, Log, TEXT("Reloading %d Packages..."), InPackagesToReload.Num());

	// Syncing may have deleted some packages, so we need to unload those rather than re-load them...
	TArray<UPackage*> PackagesToUnload;
	InPackagesToReload.RemoveAll([&](UPackage* InPackage) -> bool
	{
		const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
		if (!FPaths::FileExists(PackageFilename))
		{
			PackagesToUnload.Emplace(InPackage);
			return true; // remove package
		}
		return false; // keep package
	});

	// Hot-reload the new packages...
	UPackageTools::ReloadPackages(InPackagesToReload);

	// Unload any deleted packages...
	UPackageTools::UnloadPackages(PackagesToUnload);
}

// Ask the user if he wants to stash any modification and try to unstash them afterward, which could lead to conflicts
bool FGitSourceControlMenu::StashAwayAnyModifications()
{
	bool bStashOk = true;

	FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
	FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
	const FString& PathToRespositoryRoot = Provider.GetPathToRepositoryRoot();
	const FString& PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
	const TArray<FString> ParametersStatus{"--porcelain --untracked-files=no"};
	TArray<FString> InfoMessages;
	TArray<FString> ErrorMessages;
	// Check if there is any modification to the working tree
	const bool bStatusOk = GitSourceControlUtils::RunCommand(TEXT("status"), PathToGitBinary, PathToRespositoryRoot, ParametersStatus, TArray<FString>(), InfoMessages, ErrorMessages);
	if ((bStatusOk) && (InfoMessages.Num() > 0))
	{
		// Ask the user before stashing
		const FText DialogText(LOCTEXT("SourceControlMenu_Stash_Ask", "Stash (save) all modifications of the working tree? Required to Sync/Pull!"));
		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
		if (Choice == EAppReturnType::Ok)
		{
			const TArray<FString> ParametersStash{ "save \"Stashed by Unreal Engine Git Plugin\"" };
			bStashMadeBeforeSync = GitSourceControlUtils::RunCommand(TEXT("stash"), PathToGitBinary, PathToRespositoryRoot, ParametersStash, TArray<FString>(), InfoMessages, ErrorMessages);
			if (!bStashMadeBeforeSync)
			{
				FMessageLog SourceControlLog("SourceControl");
				SourceControlLog.Warning(LOCTEXT("SourceControlMenu_StashFailed", "Stashing away modifications failed!"));
				SourceControlLog.Notify();
			}
		}
		else
		{
			bStashOk = false;
		}
	}

	return bStashOk;
}

// Unstash any modifications if a stash was made at the beginning of the Sync operation
void FGitSourceControlMenu::ReApplyStashedModifications()
{
	if (bStashMadeBeforeSync)
	{
		FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		const FString& PathToRespositoryRoot = Provider.GetPathToRepositoryRoot();
		const FString& PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();
		const TArray<FString> ParametersStash{ "pop" };
		TArray<FString> InfoMessages;
		TArray<FString> ErrorMessages;
		const bool bUnstashOk = GitSourceControlUtils::RunCommand(TEXT("stash"), PathToGitBinary, PathToRespositoryRoot, ParametersStash, TArray<FString>(), InfoMessages, ErrorMessages);
		if (!bUnstashOk)
		{
			FMessageLog SourceControlLog("SourceControl");
			SourceControlLog.Warning(LOCTEXT("SourceControlMenu_UnstashFailed", "Unstashing previously saved modifications failed!"));
			SourceControlLog.Notify();
		}
	}
}

void FGitSourceControlMenu::SyncClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Ask the user to save any dirty assets opened in Editor
		const bool bSaved = SaveDirtyPackages();
		if (bSaved)
		{
			// Better way to do a pull/sync
			// First fetch and check what files have really changed
			// Then unload only the necessary packages instead of everything
			// Then reload the changed package.
			FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
			FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
			const FString& PathToRepositoryRoot = Provider.GetPathToRepositoryRoot();
			const FString& PathToGitBinary = GitSourceControl.AccessSettings().GetBinaryPath();

			FString BranchName;
			GitSourceControlUtils::GetBranchName(PathToGitBinary, PathToRepositoryRoot, BranchName);

			FString DiffOrigin("..origin/");
			DiffOrigin.Append(BranchName);

			TArray<FString> ChangedFiles;
			{
				TArray<FString> ErrorMessages;
				TArray<FString> Parameters;
				Parameters.Add(TEXT("--stat"));
				Parameters.Add(TEXT("--name-only"));
				Parameters.Add(DiffOrigin);

				// Get changed files remote and on local commits (we need to determine which ones are local commits)
				GitSourceControlUtils::RunCommand(TEXT("diff"), PathToGitBinary, PathToRepositoryRoot, Parameters, TArray<FString>(), ChangedFiles, ErrorMessages);

				// Handle uncommitted changes
				TArray<FString> LocalChangedFiles;
				const TArray<FString> ParametersStatus{"--porcelain --untracked-files=no"};
				GitSourceControlUtils::RunCommand(TEXT("status"), PathToGitBinary, PathToRepositoryRoot, ParametersStatus, TArray<FString>(), LocalChangedFiles, ErrorMessages);

				for(FString& Filename: LocalChangedFiles)
				{
					if(Filename.StartsWith(" M") || Filename.StartsWith(" A"))
					{
						ChangedFiles.Add(Filename.RightChop(3));
					}
				}
			}

			// TODO: Handle local commits
			TArray<FString> PackagesToUnlink;
			for(FString& Filename: ChangedFiles)
			{
				FString AbsolutePath = FPaths::ConvertRelativePathToFull(PathToRepositoryRoot, Filename);
				FString PackageName;
				if(FPackageName::TryConvertFilenameToLongPackageName(AbsolutePath, PackageName))
				{
					UE_LOG(LogTemp, Warning, TEXT("%s - %s"), *AbsolutePath, *PackageName);
					PackagesToUnlink.Add(PackageName);
				}
			}
		
			PackagesToReload = UnlinkPackages(PackagesToUnlink);
			
			// // Ask the user if he wants to stash any modification and try to unstash them afterward, which could lead to conflicts
			const bool bStashed = StashAwayAnyModifications();
			if (bStashed)
			{
				TSharedRef<FSync, ESPMode::ThreadSafe> SyncOperation = ISourceControlOperation::Create<FSync>();
#if ENGINE_MAJOR_VERSION == 5
				const ECommandResult::Type Result = Provider.Execute(SyncOperation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#else
				const ECommandResult::Type Result = Provider.Execute(SyncOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#endif
				if (Result == ECommandResult::Succeeded)
				{
					// Display an ongoing notification during the whole operation (packages will be reloaded at the completion of the operation)
					DisplayInProgressNotification(SyncOperation->GetInProgressString());
				}
				else
				{
					// Report failure with a notification and Reload all packages
					DisplayFailureNotification(SyncOperation->GetName());
					ReloadPackages(PackagesToReload);
				}
			}
			else
			{
				FMessageLog SourceControlLog("SourceControl");
				SourceControlLog.Warning(LOCTEXT("SourceControlMenu_Sync_Unsaved", "Stash away all modifications before attempting to Sync!"));
				SourceControlLog.Notify();
			}
		}
		else
		{
			FMessageLog SourceControlLog("SourceControl");
			SourceControlLog.Warning(LOCTEXT("SourceControlMenu_Sync_Unsaved", "Save All Assets before attempting to Sync!"));
			SourceControlLog.Notify();
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

void FGitSourceControlMenu::PushClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch a "Push" Operation
		FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		TSharedRef<FGitPush, ESPMode::ThreadSafe> PushOperation = ISourceControlOperation::Create<FGitPush>();
#if ENGINE_MAJOR_VERSION == 5
		const ECommandResult::Type Result = Provider.Execute(PushOperation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#else
		const ECommandResult::Type Result = Provider.Execute(PushOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#endif
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(PushOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(PushOperation->GetName());
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

void FGitSourceControlMenu::RevertClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Ask the user before reverting all!
		const FText DialogText(LOCTEXT("SourceControlMenu_Revert_Ask", "Revert all modifications of the working tree?"));
		const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::OkCancel, DialogText);
		if (Choice == EAppReturnType::Ok)
		{
			// NOTE No need to force the user to SaveDirtyPackages(); since he will be presented with a choice by the Editor

			// Find and Unlink all packages in Content directory to allow to update them
			PackagesToReload = UnlinkPackages(ListAllPackages());

			// Launch a "Revert" Operation
			FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
			FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
			TSharedRef<FRevert, ESPMode::ThreadSafe> RevertOperation = ISourceControlOperation::Create<FRevert>();
#if ENGINE_MAJOR_VERSION == 5
			const ECommandResult::Type Result = Provider.Execute(RevertOperation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#else
			const ECommandResult::Type Result = Provider.Execute(RevertOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#endif
			if (Result == ECommandResult::Succeeded)
			{
				// Display an ongoing notification during the whole operation
				DisplayInProgressNotification(RevertOperation->GetInProgressString());
			}
			else
			{
				// Report failure with a notification and Reload all packages
				DisplayFailureNotification(RevertOperation->GetName());
				ReloadPackages(PackagesToReload);
			}
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

void FGitSourceControlMenu::RefreshClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch an "UpdateStatus" Operation
		FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FUpdateStatus>();
		RefreshOperation->SetCheckingAllFiles(true);
#if ENGINE_MAJOR_VERSION == 5
		const ECommandResult::Type Result = Provider.Execute(RefreshOperation, FSourceControlChangelistPtr(), TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#else
		const ECommandResult::Type Result = Provider.Execute(RefreshOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
#endif
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(RefreshOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(RefreshOperation->GetName());
		}
	}
	else
	{
		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		SourceControlLog.Notify();
	}
}

// Display an ongoing notification during the whole operation
void FGitSourceControlMenu::DisplayInProgressNotification(const FText& InOperationInProgressString)
{
	if (!OperationInProgressNotification.IsValid())
	{
		FNotificationInfo Info(InOperationInProgressString);
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (OperationInProgressNotification.IsValid())
		{
			OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
}

// Remove the ongoing notification at the end of the operation
void FGitSourceControlMenu::RemoveInProgressNotification()
{
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
		OperationInProgressNotification.Reset();
	}
}

// Display a temporary success notification at the end of the operation
void FGitSourceControlMenu::DisplaySucessNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Success", "{0} operation was successful!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.bUseSuccessFailIcons = true;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
#else
	Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
#endif
	FSlateNotificationManager::Get().AddNotification(Info);
	UE_LOG(LogSourceControl, Log, TEXT("%s"), *NotificationText.ToString());
}

// Display a temporary failure notification at the end of the operation
void FGitSourceControlMenu::DisplayFailureNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Failure", "Error: {0} operation failed!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	UE_LOG(LogSourceControl, Error, TEXT("%s"), *NotificationText.ToString());
}

void FGitSourceControlMenu::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	RemoveInProgressNotification();

	if ((InOperation->GetName() == "Sync") || (InOperation->GetName() == "Revert"))
	{
		// Unstash any modifications if a stash was made at the beginning of the Sync operation
		ReApplyStashedModifications();
		// Reload packages that where unlinked at the beginning of the Sync/Revert operation
		ReloadPackages(PackagesToReload);
	}

	// Report result with a notification
	if (InResult == ECommandResult::Succeeded)
	{
		DisplaySucessNotification(InOperation->GetName());
	}
	else
	{
		DisplayFailureNotification(InOperation->GetName());
	}
}

#if ENGINE_MAJOR_VERSION == 5
void FGitSourceControlMenu::AddMenuExtension(FToolMenuSection& Builder)
#else
void FGitSourceControlMenu::AddMenuExtension(FMenuBuilder& Builder)
#endif
{
	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION == 5
		"GitPush",
#endif

		LOCTEXT("GitPush",				"Push"),
		LOCTEXT("GitPushTooltip",		"Push all local commits to the remote server."),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Submit"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Submit"),
#endif
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::PushClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::HaveRemoteUrl)
		)
	);

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION == 5
		"GitSync",
#endif
		LOCTEXT("GitSync",				"Sync/Pull"),
		LOCTEXT("GitSyncTooltip",		"Update all files in the local repository to the latest version of the remote server."),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Sync"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Sync"),
#endif
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::SyncClicked),
			FCanExecuteAction::CreateRaw(this, &FGitSourceControlMenu::HaveRemoteUrl)
		)
	);

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION == 5
		"GitRevert",
#endif
		LOCTEXT("GitRevert",			"Revert"),
		LOCTEXT("GitRevertTooltip",		"Revert all files in the repository to their unchanged state."),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Revert"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Revert"),
#endif
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RevertClicked),
			FCanExecuteAction()
		)
	);

	Builder.AddMenuEntry(
#if ENGINE_MAJOR_VERSION == 5
		"GitRefresh",
#endif
		LOCTEXT("GitRefresh",			"Refresh"),
		LOCTEXT("GitRefreshTooltip",	"Update the source control status of all files in the local repository."),
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Refresh"),
#else
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Refresh"),
#endif
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RefreshClicked),
			FCanExecuteAction()
		)
	);
}

#if ENGINE_MAJOR_VERSION == 4
TSharedRef<FExtender> FGitSourceControlMenu::OnExtendLevelEditorViewMenu(const TSharedRef<FUICommandList> CommandList)
{
	TSharedRef<FExtender> Extender(new FExtender());

	Extender->AddMenuExtension(
		"SourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FGitSourceControlMenu::AddMenuExtension));

	return Extender;
}
#endif

#undef LOCTEXT_NAMESPACE
