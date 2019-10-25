(* ::Package:: *)

BeginTestSection["CommitTests"]


Needs["GitLink`"];
$RepoRootDirectory = FileNameJoin[{$TemporaryDirectory, "InitRepos"}];
Quiet[DeleteDirectory[$RepoRootDirectory, DeleteContents->True]];
Quiet[CreateDirectory[$RepoRootDirectory]];
SetAttributes[gitInitBlock,HoldFirst];
gitInitBlock[code_,opts___]:=
	Block[{result, $Repo, $RepoDirectory},
		$RepoDirectory = FileNameJoin[{AbsoluteFileName[$RepoRootDirectory],"InitTestRepo"}];
		$Repo=GitInit[$RepoDirectory,opts];
		result=code;
		GitClose[$Repo];
		DeleteDirectory[$RepoDirectory,DeleteContents->True];
		result
	]


VerificationTest[
	gitInitBlock[
		Module[{c},
		CreateFile[FileNameJoin[{$RepoDirectory, "foo"}]];
		GitAdd[$Repo, FileNameJoin[{$RepoDirectory, "foo"}]];
		c=GitCommit[$Repo, "message"];
		Echo@{GitCommitQ[c], c["Message"], c["Parents"]}
	]]
	,
	{True, "message", {}}	
]


VerificationTest[
	gitInitBlock[
		Module[{c, t=Now},
		CreateFile[FileNameJoin[{$RepoDirectory, "foo"}]];
		GitAdd[$Repo, FileNameJoin[{$RepoDirectory, "foo"}]];
		c=GitCommit[$Repo, "message", "AuthorSignature"-><|"Name"->"j", "Email"->"k", "TimeStamp"->t|>];
		{GitCommitQ[c], c["Message"], c["Parents"], c["Author"]["Name"], c["Author"]["Email"], c["Author"]["TimeStamp"] === t}
	]]
	,
	{True, "message", {}, "j", "k", True}	
]


EndTestSection[]
