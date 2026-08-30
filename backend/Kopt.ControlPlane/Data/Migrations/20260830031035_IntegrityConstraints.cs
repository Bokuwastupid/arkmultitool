using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Kopt.ControlPlane.Data.Migrations
{
    /// <inheritdoc />
    public partial class IntegrityConstraints : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropIndex(
                name: "IX_Subscriptions_UserId_ProductId",
                table: "Subscriptions");

            migrationBuilder.CreateIndex(
                name: "IX_TamperIncidents_DeviceId",
                table: "TamperIncidents",
                column: "DeviceId");

            migrationBuilder.CreateIndex(
                name: "IX_TamperIncidents_ResolvedAt_CreatedAt",
                table: "TamperIncidents",
                columns: new[] { "ResolvedAt", "CreatedAt" });

            migrationBuilder.CreateIndex(
                name: "IX_TamperIncidents_UserId",
                table: "TamperIncidents",
                column: "UserId");

            migrationBuilder.AddCheckConstraint(
                name: "CK_TamperIncident_RiskScore",
                table: "TamperIncidents",
                sql: "\"RiskScore\" BETWEEN 0 AND 100");

            migrationBuilder.CreateIndex(
                name: "IX_Subscriptions_ProductId",
                table: "Subscriptions",
                column: "ProductId");

            migrationBuilder.CreateIndex(
                name: "IX_Subscriptions_UserId_ProductId",
                table: "Subscriptions",
                columns: new[] { "UserId", "ProductId" },
                unique: true);

            migrationBuilder.AddCheckConstraint(
                name: "CK_Subscription_SlotLimit",
                table: "Subscriptions",
                sql: "\"SlotLimit\" BETWEEN 1 AND 20");

            migrationBuilder.AddCheckConstraint(
                name: "CK_ProductRelease_RolloutPercent",
                table: "Releases",
                sql: "\"RolloutPercent\" BETWEEN 0 AND 100");

            migrationBuilder.CreateIndex(
                name: "IX_LicenseKeys_ProductId",
                table: "LicenseKeys",
                column: "ProductId");

            migrationBuilder.AddCheckConstraint(
                name: "CK_LicenseKey_DurationDays",
                table: "LicenseKeys",
                sql: "\"DurationDays\" BETWEEN 1 AND 3650");

            migrationBuilder.AddCheckConstraint(
                name: "CK_LicenseKey_SlotLimit",
                table: "LicenseKeys",
                sql: "\"SlotLimit\" BETWEEN 1 AND 20");

            migrationBuilder.CreateIndex(
                name: "IX_Leases_DeviceId",
                table: "Leases",
                column: "DeviceId");

            migrationBuilder.CreateIndex(
                name: "IX_Leases_SubscriptionId",
                table: "Leases",
                column: "SubscriptionId");

            migrationBuilder.CreateIndex(
                name: "IX_Devices_ProductId",
                table: "Devices",
                column: "ProductId");

            migrationBuilder.CreateIndex(
                name: "IX_AuditEvents_CreatedAt",
                table: "AuditEvents",
                column: "CreatedAt");

            migrationBuilder.AddForeignKey(
                name: "FK_Devices_AspNetUsers_UserId",
                table: "Devices",
                column: "UserId",
                principalTable: "AspNetUsers",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_Devices_Products_ProductId",
                table: "Devices",
                column: "ProductId",
                principalTable: "Products",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_Leases_Devices_DeviceId",
                table: "Leases",
                column: "DeviceId",
                principalTable: "Devices",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_Leases_Subscriptions_SubscriptionId",
                table: "Leases",
                column: "SubscriptionId",
                principalTable: "Subscriptions",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_LicenseKeys_Products_ProductId",
                table: "LicenseKeys",
                column: "ProductId",
                principalTable: "Products",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_Releases_Products_ProductId",
                table: "Releases",
                column: "ProductId",
                principalTable: "Products",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_Subscriptions_AspNetUsers_UserId",
                table: "Subscriptions",
                column: "UserId",
                principalTable: "AspNetUsers",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_Subscriptions_Products_ProductId",
                table: "Subscriptions",
                column: "ProductId",
                principalTable: "Products",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_TamperIncidents_AspNetUsers_UserId",
                table: "TamperIncidents",
                column: "UserId",
                principalTable: "AspNetUsers",
                principalColumn: "Id",
                onDelete: ReferentialAction.Restrict);

            migrationBuilder.AddForeignKey(
                name: "FK_TamperIncidents_Devices_DeviceId",
                table: "TamperIncidents",
                column: "DeviceId",
                principalTable: "Devices",
                principalColumn: "Id",
                onDelete: ReferentialAction.SetNull);
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropForeignKey(
                name: "FK_Devices_AspNetUsers_UserId",
                table: "Devices");

            migrationBuilder.DropForeignKey(
                name: "FK_Devices_Products_ProductId",
                table: "Devices");

            migrationBuilder.DropForeignKey(
                name: "FK_Leases_Devices_DeviceId",
                table: "Leases");

            migrationBuilder.DropForeignKey(
                name: "FK_Leases_Subscriptions_SubscriptionId",
                table: "Leases");

            migrationBuilder.DropForeignKey(
                name: "FK_LicenseKeys_Products_ProductId",
                table: "LicenseKeys");

            migrationBuilder.DropForeignKey(
                name: "FK_Releases_Products_ProductId",
                table: "Releases");

            migrationBuilder.DropForeignKey(
                name: "FK_Subscriptions_AspNetUsers_UserId",
                table: "Subscriptions");

            migrationBuilder.DropForeignKey(
                name: "FK_Subscriptions_Products_ProductId",
                table: "Subscriptions");

            migrationBuilder.DropForeignKey(
                name: "FK_TamperIncidents_AspNetUsers_UserId",
                table: "TamperIncidents");

            migrationBuilder.DropForeignKey(
                name: "FK_TamperIncidents_Devices_DeviceId",
                table: "TamperIncidents");

            migrationBuilder.DropIndex(
                name: "IX_TamperIncidents_DeviceId",
                table: "TamperIncidents");

            migrationBuilder.DropIndex(
                name: "IX_TamperIncidents_ResolvedAt_CreatedAt",
                table: "TamperIncidents");

            migrationBuilder.DropIndex(
                name: "IX_TamperIncidents_UserId",
                table: "TamperIncidents");

            migrationBuilder.DropCheckConstraint(
                name: "CK_TamperIncident_RiskScore",
                table: "TamperIncidents");

            migrationBuilder.DropIndex(
                name: "IX_Subscriptions_ProductId",
                table: "Subscriptions");

            migrationBuilder.DropIndex(
                name: "IX_Subscriptions_UserId_ProductId",
                table: "Subscriptions");

            migrationBuilder.DropCheckConstraint(
                name: "CK_Subscription_SlotLimit",
                table: "Subscriptions");

            migrationBuilder.DropCheckConstraint(
                name: "CK_ProductRelease_RolloutPercent",
                table: "Releases");

            migrationBuilder.DropIndex(
                name: "IX_LicenseKeys_ProductId",
                table: "LicenseKeys");

            migrationBuilder.DropCheckConstraint(
                name: "CK_LicenseKey_DurationDays",
                table: "LicenseKeys");

            migrationBuilder.DropCheckConstraint(
                name: "CK_LicenseKey_SlotLimit",
                table: "LicenseKeys");

            migrationBuilder.DropIndex(
                name: "IX_Leases_DeviceId",
                table: "Leases");

            migrationBuilder.DropIndex(
                name: "IX_Leases_SubscriptionId",
                table: "Leases");

            migrationBuilder.DropIndex(
                name: "IX_Devices_ProductId",
                table: "Devices");

            migrationBuilder.DropIndex(
                name: "IX_AuditEvents_CreatedAt",
                table: "AuditEvents");

            migrationBuilder.CreateIndex(
                name: "IX_Subscriptions_UserId_ProductId",
                table: "Subscriptions",
                columns: new[] { "UserId", "ProductId" });
        }
    }
}
