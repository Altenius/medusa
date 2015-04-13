#include "medusa/document.hpp"
#include "medusa/medusa.hpp"
#include "medusa/value.hpp"
#include "medusa/log.hpp"
#include "medusa/module.hpp"

#include <boost/bind.hpp>
#include <boost/foreach.hpp>

MEDUSA_NAMESPACE_BEGIN

Document::Document(void)
: m_AddressHistoryIndex()
{
}

Document::~Document(void)
{
  if (m_spDatabase)
    m_spDatabase->Close();
  m_QuitSignal();
  RemoveAll();
}

bool Document::Use(Database::SPType spDb)
{
  if (m_spDatabase)
    return false;
  m_spDatabase = spDb;
  return true;
}

bool Document::Flush(void)
{
  return m_spDatabase->Flush();
}

void Document::RemoveAll(void)
{
  std::lock_guard<MutexType> Lock(m_CellMutex);
  for (auto MultiCellPair : m_MultiCells)
    delete MultiCellPair.second;
  m_MultiCells.clear();
  m_QuitSignal.disconnect_all_slots();
  m_DocumentUpdatedSignal.disconnect_all_slots();
  m_MemoryAreaUpdatedSignal.disconnect_all_slots();
  m_AddressUpdatedSignal.disconnect_all_slots();
  m_LabelUpdatedSignal.disconnect_all_slots();
  m_TaskUpdatedSignal.disconnect_all_slots();
}

void Document::Connect(u32 Type, Document::Subscriber* pSubscriber)
{
  if (Type & Subscriber::Quit)
    pSubscriber->m_QuitConnection = m_QuitSignal.connect(boost::bind(&Subscriber::OnQuit, pSubscriber));

  if (Type & Subscriber::DocumentUpdated)
    pSubscriber->m_DocumentUpdatedConnection = m_DocumentUpdatedSignal.connect(boost::bind(&Subscriber::OnDocumentUpdated, pSubscriber));

  if (Type & Subscriber::MemoryAreaUpdated)
    pSubscriber->m_MemoryAreaUpdatedConnection = m_MemoryAreaUpdatedSignal.connect(boost::bind(&Subscriber::OnMemoryAreaUpdated, pSubscriber, _1, _2));

  if (Type & Subscriber::AddressUpdated)
    pSubscriber->m_AddressUpdatedConnection = m_AddressUpdatedSignal.connect(boost::bind(&Subscriber::OnAddressUpdated, pSubscriber, _1));

  if (Type & Subscriber::LabelUpdated)
    pSubscriber->m_LabelUpdatedConnection = m_LabelUpdatedSignal.connect(boost::bind(&Subscriber::OnLabelUpdated, pSubscriber, _1, _2, _3));

  if (Type & Subscriber::TaskUpdated)
    pSubscriber->m_TaskUpdatedConnection = m_TaskUpdatedSignal.connect(boost::bind(&Subscriber::OnTaskUpdated, pSubscriber, _1, _2));
}

MemoryArea const* Document::GetMemoryArea(Address const& rAddr) const
{
  if (m_spDatabase == nullptr)
    return nullptr;
  return m_spDatabase->GetMemoryArea(rAddr);
}

Label Document::GetLabelFromAddress(Address const& rAddr) const
{
  Label CurLbl;
  if (m_spDatabase == nullptr)
    return Label();
  m_spDatabase->GetLabel(rAddr, CurLbl);
  return CurLbl;
}

void Document::SetLabelToAddress(Address const& rAddr, Label const& rLabel)
{
  AddLabel(rAddr, rLabel, true);
}

Address Document::GetAddressFromLabelName(std::string const& rLabelName) const
{
  Address LblAddr;
  if (m_spDatabase == nullptr)
    return Address();
  m_spDatabase->GetLabelAddress(rLabelName, LblAddr);
  return LblAddr;
}

void Document::AddLabel(Address const& rAddr, Label const& rLabel, bool Force)
{
  if (m_spDatabase == nullptr)
    return;
  if (rLabel.GetName().empty() && Force)
  {
    RemoveLabel(rAddr);
    return;
  }

  Label OldLbl, NewLbl = rLabel;
  Address Addr;
  if (m_spDatabase->GetLabelAddress(NewLbl, Addr))
  {
    do NewLbl.IncrementVersion();
    while (m_spDatabase->GetLabelAddress(NewLbl, Addr));
  }

  if (m_spDatabase->GetLabel(rAddr, OldLbl) == true)
  {
    if (OldLbl.IsAutoGenerated())
      Force = true;

    if (!Force)
      return;

    if (OldLbl == rLabel)
      return;

    if (!m_spDatabase->RemoveLabel(rAddr))
      return;

    m_LabelUpdatedSignal(rAddr, OldLbl, true);
  }

  m_spDatabase->AddLabel(rAddr, NewLbl);
  m_LabelUpdatedSignal(rAddr, NewLbl, false);
  m_DocumentUpdatedSignal();
}

void Document::RemoveLabel(Address const& rAddr)
{
  if (m_spDatabase == nullptr)
    return;
  Label CurLbl;
  m_spDatabase->GetLabel(rAddr, CurLbl);
  m_spDatabase->RemoveLabel(rAddr);
  m_LabelUpdatedSignal(rAddr, CurLbl, true);
  m_DocumentUpdatedSignal();
}

void Document::ForEachLabel(Database::LabelCallback Callback) const
{
  if (m_spDatabase == nullptr)
    return;
  m_spDatabase->ForEachLabel(Callback);
}

bool Document::AddCrossReference(Address const& rTo, Address const& rFrom)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->AddCrossReference(rTo, rFrom);
}

bool Document::RemoveCrossReference(Address const& rFrom)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->RemoveCrossReference(rFrom);
}

bool Document::RemoveCrossReferences(void)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->RemoveCrossReferences();
}

bool Document::HasCrossReferenceFrom(Address const& rTo) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->HasCrossReferenceFrom(rTo);
}

bool Document::GetCrossReferenceFrom(Address const& rTo, Address::List& rFromList) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->GetCrossReferenceFrom(rTo, rFromList);
}

bool Document::HasCrossReferenceTo(Address const& rFrom) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->HasCrossReferenceTo(rFrom);
}

bool Document::GetCrossReferenceTo(Address const& rFrom, Address::List& rToList) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->GetCrossReferenceTo(rFrom, rToList);
}

bool Document::ChangeValueSize(Address const& rValueAddr, u8 NewValueSize, bool Force)
{
  if (NewValueSize == 0x0)
    return false;

  Cell::SPType spOldCell = GetCell(rValueAddr);

  if (spOldCell == nullptr)
    return false;

  if (spOldCell->GetType() == Cell::InstructionType && Force == false)
    return false;

  NewValueSize /= 8;

  size_t OldCellLength = spOldCell->GetLength();
  if (spOldCell->GetType() == Cell::ValueType && OldCellLength == NewValueSize)
    return true;

  auto spNewCell = std::make_shared<Value>(spOldCell->GetSubType(), NewValueSize);

  if (NewValueSize > OldCellLength)
    return SetCell(rValueAddr, spNewCell, Force);

  if (SetCell(rValueAddr, spNewCell, Force) == false)
    return false;

  for (u32 i = NewValueSize; i < OldCellLength; ++i)
    if (SetCell(rValueAddr + i, std::make_shared<Value>(), Force) == false)
      return false;

  return true;
}

bool Document::MakeString(Address const& rAddress, u8 StringType, u16 StringLength, bool Force)
{
  TOffset FileOff;
  if (!ConvertAddressToFileOffset(rAddress, FileOff))
    return false;
  u16 StrLen = GetBinaryStream().StringLength(FileOff);
  if (StrLen == 0)
    return false;
  if (StrLen > StringLength)
    return false;
  ++StrLen; // we want to include '\0'
  auto spNewStr = std::make_shared<String>(StringType, std::min(StrLen, StringLength));
  return SetCell(rAddress, spNewStr, Force);
}

bool Document::GetComment(Address const& rAddress, std::string& rComment) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->GetComment(rAddress, rComment);
}

bool Document::SetComment(Address const& rAddress, std::string const& rComment)
{
  if (m_spDatabase == nullptr)
    return false;
  if (m_spDatabase->SetComment(rAddress, rComment))
  {
    m_DocumentUpdatedSignal();
    return true;
  }
  return false;
}

Cell::SPType Document::GetCell(Address const& rAddr)
{
  if (m_spDatabase == nullptr)
    return nullptr;
  std::lock_guard<MutexType> Lock(m_CellMutex);

  CellData CurCellData;
  if (!m_spDatabase->GetCellData(rAddr, CurCellData))
    return nullptr;
  auto spCellData = std::make_shared<CellData>(CurCellData); // TODO: we can avoid this

  switch (CurCellData.GetType())
  {
  case Cell::ValueType:     return std::make_shared<Value>(spCellData);
  case Cell::CharacterType: return std::make_shared<Character>(spCellData);
  case Cell::StringType:    return std::make_shared<String>(spCellData);
  case Cell::InstructionType:
    {
      auto spInsn = std::make_shared<Instruction>();
      spInsn->GetData()->ArchitectureTag() = CurCellData.GetArchitectureTag();
      spInsn->Mode() = CurCellData.GetMode();
      auto spArch = ModuleManager::Instance().GetArchitecture(CurCellData.GetArchitectureTag());
      if (spArch == nullptr)
      {
        Log::Write("core") << "unable to get architecture for " << rAddr << LogEnd;
        return nullptr;
      }
      TOffset Offset;
      ConvertAddressToFileOffset(rAddr, Offset);
      spArch->Disassemble(GetBinaryStream(), Offset, *spInsn, CurCellData.GetMode());
      return spInsn;
    }
  default:
    break;
  }

  return Cell::SPType();
}

Cell::SPType const Document::GetCell(Address const& rAddr) const
{
  if (m_spDatabase == nullptr)
    return nullptr;
  std::lock_guard<MutexType> Lock(m_CellMutex);

  CellData CurCellData;
  if (!m_spDatabase->GetCellData(rAddr, CurCellData))
    return nullptr;
  auto spCellData = std::make_shared<CellData>(CurCellData); // TODO: we can avoid this

  switch (CurCellData.GetType())
  {
  case Cell::ValueType:     return std::make_shared<Value>(spCellData);
  case Cell::CharacterType: return std::make_shared<Character>(spCellData);
  case Cell::StringType:    return std::make_shared<String>(spCellData);
  case Cell::InstructionType:
    {
      auto spInsn = std::make_shared<Instruction>();
      spInsn->GetData()->ArchitectureTag() = CurCellData.GetArchitectureTag();
      spInsn->Mode() = CurCellData.GetMode();
      auto spArch = ModuleManager::Instance().GetArchitecture(CurCellData.GetArchitectureTag());
      if (spArch == nullptr)
      {
        Log::Write("core") << "unable to get architecture for " << rAddr << LogEnd;
        return nullptr;
      }
      TOffset Offset;
      ConvertAddressToFileOffset(rAddr, Offset);
      spArch->Disassemble(GetBinaryStream(), Offset, *spInsn, CurCellData.GetMode());
      return spInsn;
    }
  default:
    break;
  }

  return Cell::SPType();
}

u8 Document::GetCellType(Address const& rAddr) const
{
  if (m_spDatabase == nullptr)
    return Cell::CellType;
  CellData CurCellData;
  if (!m_spDatabase->GetCellData(rAddr, CurCellData))
    return Cell::CellType;
  return CurCellData.GetType();
}

u8 Document::GetCellSubType(Address const& rAddr) const
{
  if (m_spDatabase == nullptr)
    return Cell::CellType;
  CellData CurCellData;
  if (!m_spDatabase->GetCellData(rAddr, CurCellData))
    return Cell::CellType;
  return CurCellData.GetSubType();
}

bool Document::SetCell(Address const& rAddr, Cell::SPType spCell, bool Force)
{
  if (m_spDatabase == nullptr)
    return false;
  Address::List ErasedAddresses;
  if (!m_spDatabase->SetCellData(rAddr, *spCell->GetData(), ErasedAddresses, Force))
    return false;

  RemoveLabelIfNeeded(rAddr);

  for (Address const& rErsdAddr : ErasedAddresses)
    if (GetCell(rErsdAddr) == nullptr)
    {
      if (HasCrossReferenceTo(rErsdAddr))
        RemoveCrossReference(rErsdAddr);

      if (HasCrossReferenceFrom(rErsdAddr))
      {
        auto Label = GetLabelFromAddress(rErsdAddr);
        if (Label.GetType() != Label::Unknown)
        {
          m_LabelUpdatedSignal(rErsdAddr, Label, true);
        }
      }
    }

  Address::List AddressList;
  AddressList.push_back(rAddr);
  AddressList.merge(ErasedAddresses);

  m_DocumentUpdatedSignal();
  m_AddressUpdatedSignal(AddressList);

  return true;
}

bool Document::SetCellWithLabel(Address const& rAddr, Cell::SPType spCell, Label const& rLabel, bool Force)
{
  if (m_spDatabase == nullptr)
    return false;
  Address::List ErasedAddresses;
  if (!m_spDatabase->SetCellData(rAddr, *spCell->GetData(), ErasedAddresses, Force))
    return false;

  RemoveLabelIfNeeded(rAddr);

  for (Address const& rErsdAddr : ErasedAddresses)
    if (GetCell(rErsdAddr) == nullptr)
    {
      if (HasCrossReferenceTo(rErsdAddr))
        RemoveCrossReference(rErsdAddr);

      if (HasCrossReferenceFrom(rErsdAddr))
      {
        auto Label = GetLabelFromAddress(rErsdAddr);
        if (Label.GetType() != Label::Unknown)
        {
          m_LabelUpdatedSignal(rErsdAddr, Label, true);
        }
      }
    }

  Address::List AddressList;
  AddressList.push_back(rAddr);
  AddressList.merge(ErasedAddresses);

  Label OldLabel;
  if (m_spDatabase->GetLabel(rAddr, OldLabel) == true)
  {
    if (!Force)
      return false;

    if (OldLabel == rLabel)
      return true;

    if (!m_spDatabase->RemoveLabel(rAddr))
      return false;

    m_LabelUpdatedSignal(rAddr, OldLabel, true);
  }
  m_spDatabase->AddLabel(rAddr, rLabel);

  m_LabelUpdatedSignal(rAddr, rLabel, false);
  m_DocumentUpdatedSignal();
  m_AddressUpdatedSignal(AddressList);

  return true;
}

bool Document::DeleteCell(Address const& rAddr)
{
  if (m_spDatabase == nullptr)
    return false;
  if (!m_spDatabase->DeleteCellData(rAddr))
    return false;

  Address::List DelAddr;
  DelAddr.push_back(rAddr);
  m_AddressUpdatedSignal(DelAddr);
  m_DocumentUpdatedSignal();
  RemoveLabelIfNeeded(rAddr);

  return true;
}

MultiCell* Document::GetMultiCell(Address const& rAddr)
{
  // TODO: Use database here
  MultiCell::Map::iterator itMultiCell = m_MultiCells.find(rAddr);
  if (itMultiCell == m_MultiCells.end())
    return nullptr;

  return itMultiCell->second;
}

MultiCell const* Document::GetMultiCell(Address const& rAddr) const
{
  // TODO: Use database here
  MultiCell::Map::const_iterator itMultiCell = m_MultiCells.find(rAddr);
  if (itMultiCell == m_MultiCells.end())
    return nullptr;

  return itMultiCell->second;
}

bool Document::SetMultiCell(Address const& rAddr, MultiCell* pMultiCell, bool Force)
{
  if (Force == false)
  {
    MultiCell::Map::iterator itMultiCell = m_MultiCells.find(rAddr);
    if (itMultiCell != m_MultiCells.end())
    {
      delete pMultiCell; // FIXME: multicell must be redesigned to avoid this situation...
      return false;
    }
  }

  m_MultiCells[rAddr] = pMultiCell;
  m_spDatabase->AddMultiCell(rAddr, *pMultiCell);

  m_DocumentUpdatedSignal();
  Address::List AddressList;
  AddressList.push_back(rAddr);
  m_AddressUpdatedSignal(AddressList);

  if (pMultiCell->GetType() == MultiCell::StructType)
  {
    auto StructId = pMultiCell->GetId();
    StructureDetail StructDtl;
    if (!GetStructureDetail(StructId, StructDtl))
      return true;
    if (!_ApplyStructure(rAddr, StructDtl))
    {
      Log::Write("core") << "failed to apply structure at " << rAddr << LogEnd;
    }
  }

  return true;
}

bool Document::GetValueDetail(Id ConstId, ValueDetail& rConstDtl) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->GetValueDetail(ConstId, rConstDtl);
}

bool Document::SetValueDetail(Id ConstId, ValueDetail const& rConstDtl)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->SetValueDetail(ConstId, rConstDtl);
}

bool Document::GetFunctionDetail(Id FuncId, FunctionDetail& rFuncDtl) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->GetFunctionDetail(FuncId, rFuncDtl);
}

bool Document::SetFunctionDetail(Id FuncId, FunctionDetail const& rFuncDtl)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->SetFunctionDetail(FuncId, rFuncDtl);
}

bool Document::GetStructureDetail(Id StructId, StructureDetail& rStructDtl) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->GetStructureDetail(StructId, rStructDtl);
}

bool Document::SetStructureDetail(Id StructId, StructureDetail const& rStructDtl)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->SetStructureDetail(StructId, rStructDtl);
}

bool Document::RetrieveDetailId(Address const& rAddress, u8 Index, Id& rDtlId) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->RetrieveDetailId(rAddress, Index, rDtlId);
}

bool Document::BindDetailId(Address const& rAddress, u8 Index, Id DtlId)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->BindDetailId(rAddress, Index, DtlId);
}

bool Document::UnbindDetailId(Address const& rAddress, u8 Index)
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->UnbindDetailId(rAddress, Index);
}

Address Document::MakeAddress(TBase Base, TOffset Offset) const
{
  MemoryArea const* pMemArea = GetMemoryArea(Address(Base, Offset));
  if (pMemArea == nullptr)
    return Address();
  return pMemArea->MakeAddress(Offset);
}

bool Document::GetPreviousAddressInHistory(Address& rAddress)
{
  std::lock_guard<MutexType> Lock(m_AddressHistoryMutex);

  if (m_AddressHistoryIndex == 0)
    return false;

  --m_AddressHistoryIndex;
  rAddress = m_AddressHistory[m_AddressHistoryIndex];
  return true;
}

bool Document::GetNextAddressInHistory(Address& rAddress)
{
  std::lock_guard<MutexType> Lock(m_AddressHistoryMutex);

  if (m_AddressHistoryIndex + 1 >= m_AddressHistory.size())
    return false;

  rAddress = m_AddressHistory[m_AddressHistoryIndex];
  ++m_AddressHistoryIndex;
  return true;
}

// LATER: it could be better to keep next addresses, but we've to limit the amount of addresses in the container
void Document::InsertAddressInHistory(Address const& rAddress)
{
  std::lock_guard<MutexType> Lock(m_AddressHistoryMutex);

  if (!m_AddressHistory.empty() && m_AddressHistory.back() == rAddress)
    return;

  if (m_AddressHistoryIndex + 1< m_AddressHistory.size())
    m_AddressHistory.erase(std::begin(m_AddressHistory) + m_AddressHistoryIndex + 1, std::end(m_AddressHistory));
  m_AddressHistory.push_back(rAddress);
  if (!m_AddressHistory.empty())
    m_AddressHistoryIndex = m_AddressHistory.size() - 1;
}

bool Document::ConvertAddressToFileOffset(Address const& rAddr, TOffset& rFileOffset) const
{
  MemoryArea const* pMemoryArea = GetMemoryArea(rAddr);
  if (pMemoryArea == nullptr)
    return false;

  return pMemoryArea->ConvertOffsetToFileOffset(rAddr.GetOffset(), rFileOffset);
}

bool Document::ConvertAddressToPosition(Address const& rAddr, u32& rPosition) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->ConvertAddressToPosition(rAddr, rPosition);
}

bool Document::ConvertPositionToAddress(u32 Position, Address& rAddr) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->ConvertPositionToAddress(Position, rAddr);
}

Address Document::GetStartAddress(void) const
{
  if (m_spDatabase == nullptr)
    return Address();
  Address StartAddr;
  if (m_spDatabase->GetLabelAddress(std::string("start"), StartAddr))
    return StartAddr;
  m_spDatabase->GetFirstAddress(StartAddr);
  return StartAddr;
}

Address Document::GetFirstAddress(void) const
{
  if (m_spDatabase == nullptr)
    return Address();
  Address FirstAddr;
  m_spDatabase->GetFirstAddress(FirstAddr);
  return FirstAddr;
}

Address Document::GetLastAddress(void) const
{
  if (m_spDatabase == nullptr)
    return Address();
  Address LastAddr;
  m_spDatabase->GetLastAddress(LastAddr);
  return LastAddr;
}

u32 Document::GetNumberOfAddress(void) const
{
  if (m_spDatabase == nullptr)
    return 0;
  u32 Res = 0;
  m_spDatabase->ForEachMemoryArea([&Res](MemoryArea const& rMemArea)
  {
    Res += static_cast<u32>(rMemArea.GetSize());
  });
  return Res;
}

bool Document::ContainsData(Address const& rAddress) const
{
  return GetCellType(rAddress) != Cell::InstructionType;
}

bool Document::ContainsCode(Address const& rAddress) const
{
  return GetCellType(rAddress) == Cell::InstructionType;
}

bool Document::ContainsUnknown(Address const& rAddress) const
{
  if (m_spDatabase == nullptr)
    return false;
  CellData CurCellData;
  if (!m_spDatabase->GetCellData(rAddress, CurCellData))
    return false;

  return CurCellData.GetType() == Cell::ValueType && CurCellData.GetLength() == 1;
}

Tag Document::GetArchitectureTag(Address const& rAddress) const
{
  Tag ArchTag = MEDUSA_ARCH_UNK;

  auto const spCell = GetCell(rAddress);
  if (spCell != nullptr)
  {
    ArchTag = spCell->GetArchitectureTag();
    if (ArchTag != MEDUSA_ARCH_UNK)
      return ArchTag;
  }
  auto const pMemArea = GetMemoryArea(rAddress);
  if (pMemArea != nullptr)
  {
    ArchTag = pMemArea->GetArchitectureTag();
    if (ArchTag != MEDUSA_ARCH_UNK)
      return ArchTag;
  }

  return ArchTag;
}

std::list<Tag> Document::GetArchitectureTags(void) const
{
  if (m_spDatabase == nullptr)
    return std::list<Tag>();
  return m_spDatabase->GetArchitectureTags();
}

u8 Document::GetMode(Address const& rAddress) const
{
  u8 Mode = 0;

  auto const spCell = GetCell(rAddress);
  if (spCell != nullptr)
  {
    auto spCellArch = ModuleManager::Instance().GetArchitecture(spCell->GetArchitectureTag());
    if (spCellArch != nullptr)
    {
      Mode = spCellArch->GetDefaultMode(rAddress);
      if (Mode != 0)
        return Mode;
    }
    Mode = spCell->GetMode();
    if (Mode != 0)
      return Mode;
  }

  auto const pMemArea = GetMemoryArea(rAddress);
  if (pMemArea != nullptr)
  {
    auto spMemAreaArch = ModuleManager::Instance().GetArchitecture(pMemArea->GetArchitectureTag());
    if (spMemAreaArch != nullptr)
    {
      Mode = spMemAreaArch->GetDefaultMode(rAddress);
      if (Mode != 0)
        return Mode;
    }
    Mode = pMemArea->GetArchitectureMode();
    if (Mode != 0)
      return Mode;
  }

  return Mode;
}

void Document::AddMemoryArea(MemoryArea* pMemoryArea)
{
  if (m_spDatabase == nullptr)
    return;
  if (!m_spDatabase->AddMemoryArea(pMemoryArea))
  {
    Log::Write("core") << "unable to add memory area: " << pMemoryArea->Dump() << LogEnd;
    return;
  }
  m_MemoryAreaUpdatedSignal(*pMemoryArea, false);
}

void Document::ForEachMemoryArea(Database::MemoryAreaCallback Callback) const
{
  if (m_spDatabase == nullptr)
    return;
  m_spDatabase->ForEachMemoryArea(Callback);
}

bool Document::MoveAddress(Address const& rAddress, Address& rMovedAddress, s64 Offset) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->MoveAddress(rAddress, rMovedAddress, Offset);
}

bool Document::GetPreviousAddress(Address const& rAddress, Address& rPreviousAddress) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->MoveAddress(rAddress, rPreviousAddress, -1);
}

bool Document::GetNextAddress(Address const& rAddress, Address& rNextAddress) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->MoveAddress(rAddress, rNextAddress, 1);
}

bool Document::GetNearestAddress(Address const& rAddress, Address& rNearestAddress) const
{
  if (m_spDatabase == nullptr)
    return false;
  return m_spDatabase->MoveAddress(rAddress, rNearestAddress, 0);
}

void Document::RemoveLabelIfNeeded(Address const& rAddr)
{
  auto Lbl = GetLabelFromAddress(rAddr);
  if (Lbl.GetType() == Label::Unknown)
    return;
  if (Lbl.GetType() & (Label::Exported | Label::Imported))
    return;
  if (!HasCrossReferenceFrom(rAddr))
    RemoveLabel(rAddr);
}

bool Document::_ApplyStructure(Address const& rAddr, StructureDetail const& rStructDtl)
{

  rStructDtl.ForEachField([&](u32 Offset, TypedValueDetail const& rField) -> bool
  {
    Address CurFldAddr = rAddr + Offset;

    std::string Cmt;
    GetComment(CurFldAddr, Cmt);
    Cmt += "struct " + rStructDtl.GetName();
    SetComment(CurFldAddr, Cmt);

    if (!_ApplyTypedValue(rAddr, CurFldAddr, rField))
      return false;

    return true;
  });

  return true;
}

bool Document::_ApplyTypedValue(Address const& rParentAddr, Address const& rAddr, TypedValueDetail const& rTpValDtl)
{
  if ( !_ApplyType(rAddr, rTpValDtl.GetType())
    || !_ApplyValue(rAddr, rTpValDtl.GetValue()))
    return false;

  std::string Cmt;
  GetComment(rAddr, Cmt);
  if (!Cmt.empty())
    Cmt += " ";
  Cmt += rTpValDtl.GetName();

  if (!SetComment(rAddr, Cmt))
    return false;

  auto const& rValDtl = rTpValDtl.GetValue();
  auto RefId = rValDtl.GetRefId();

  switch (rValDtl.GetType())
  {
  case ValueDetail::RelativeType:
  {
    StructureDetail RefStructDtl;
    if (!GetStructureDetail(RefId, RefStructDtl))
      break;
    TOffset Pos, RefOff;
    if (!ConvertAddressToFileOffset(rAddr, Pos))
      break;
    if (!GetBinaryStream().Read(Pos, RefOff, rTpValDtl.GetSize(), true))
      break;
    if (!_ApplyStructure(rParentAddr + RefOff, RefStructDtl))
      break;

    AddCrossReference(rParentAddr + RefOff, rAddr);

    Log::Write("core") << "relative structure " << RefStructDtl.GetName() << LogEnd;
    break;
  }

  case ValueDetail::CompositeType:
  {
    StructureDetail CpsStructDtl;
    if (!GetStructureDetail(RefId, CpsStructDtl))
      break;
    if (!_ApplyStructure(rAddr, CpsStructDtl))
      break;

    Log::Write("core") << "composite structure " << CpsStructDtl.GetName() << LogEnd;
    break;
  }

  default:
    break;
  }

  return true;
}

bool Document::_ApplyType(Address const& rAddr, TypeDetail::SPType const& rspTpDtl)
{
  if (rspTpDtl->GetType() == ValueDetail::CompositeType)
    return true;
  return ChangeValueSize(rAddr, rspTpDtl->GetBitSize(), true);
}

bool Document::_ApplyValue(Address const& rAddr, ValueDetail const& rValDtl)
{
  return true;
}

std::string Document::GetOperatingSystemName(void) const
{
  if (m_spDatabase == nullptr)
    return "";
  return m_spDatabase->GetOperatingSystemName();
}

MEDUSA_NAMESPACE_END
